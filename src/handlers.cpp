/**
* @file handlers.cpp handlers for homestead
*
* Project Clearwater - IMS in the Cloud
* Copyright (C) 2013 Metaswitch Networks Ltd
*
* This program is free software: you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation, either version 3 of the License, or (at your
* option) any later version, along with the "Special Exception" for use of
* the program along with SSL, set forth below. This program is distributed
* in the hope that it will be useful, but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or FITNESS FOR
* A PARTICULAR PURPOSE. See the GNU General Public License for more
* details. You should have received a copy of the GNU General Public
* License along with this program. If not, see
* <http://www.gnu.org/licenses/>.
*
* The author can be reached by email at clearwater@metaswitch.com or by
* post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
*
* Special Exception
* Metaswitch Networks Ltd grants you permission to copy, modify,
* propagate, and distribute a work formed by combining OpenSSL with The
* Software, or a work derivative of such a combination, even if such
* copying, modification, propagation, or distribution would otherwise
* violate the terms of the GPL. You must comply with the GPL in all
* respects for all of the code used other than OpenSSL.
* "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
* Project and licensed under the OpenSSL Licenses, or a work based on such
* software and licensed under the OpenSSL Licenses.
* "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
* under which the OpenSSL Project distributes the OpenSSL toolkit software,
* as those licenses appear in the file LICENSE-OPENSSL.
*/

#include "handlers.hpp"
#include "message.hpp"
#include "log.h"

#include "rapidjson/rapidjson.h"
#include "rapidjson/writer.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

//LCOV_EXCL_START
// We don't want to actually run the handlers

void BillingTask::run()
{
  if (_req.method() != htp_method_POST)
  {
    send_http_reply(405);
    return;
  }

  bool timer_interim = false;
  if (_req.param(TIMER_INTERIM_PARAM) == "true")
  {
    timer_interim = true;
    SAS::Marker cid_assoc(trail(), MARKER_ID_SIP_CALL_ID, 0);
    cid_assoc.add_var_param(call_id());
    SAS::report_marker(cid_assoc);

    SAS::Event timer_pop(trail(), SASEvent::INTERIM_TIMER_POPPED, 0);
    SAS::report_event(timer_pop);
  }

  Message* msg = NULL;
  HTTPCode rc = parse_body(call_id(), timer_interim, _req.body(), &msg, trail());

  if (rc != HTTP_OK)
  {
    SAS::Event rejected(trail(), SASEvent::REQUEST_REJECTED_INVALID_JSON, 0);
    SAS::report_event(rejected);
    send_http_reply(rc);
  }
  else
  {
    send_http_reply(rc);

    if (msg != NULL)
    {
      LOG_DEBUG("Handle the received message");

      // The session manager takes ownership of the message object and is
      // responsible for deleting it.
      _sess_mgr->handle(msg);
      msg = NULL;
    }
  }

  delete this;
}
//LCOV_EXCL_STOP

HTTPCode BillingTask::parse_body(std::string call_id,
                                 bool timer_interim,
                                 std::string reqbody,
                                 Message** msg,
                                 SAS::TrailId trail)
{
  rapidjson::Document* body = new rapidjson::Document();
  std::string bodys = reqbody;
  body->Parse<0>(bodys.c_str());
  std::vector<std::string> ccfs;
  uint32_t session_refresh_time = 0;
  role_of_node_t role_of_node;
  node_functionality_t node_functionality;

  // Log the body early so we still see it if we later determine it's invalid.
  if (Log::enabled(Log::DEBUG_LEVEL))
  {
    if (body->HasParseError())
    {
      // Print the body from the source string.  We can't pretty print an
      // invalid document.
      LOG_DEBUG("Handling request, Body:\n%s", reqbody.c_str());
    }
    else
    {
      rapidjson::StringBuffer s;
      rapidjson::PrettyWriter<rapidjson::StringBuffer> w(s);
      body->Accept(w);
      LOG_DEBUG("Handling request, body:\n%s", s.GetString());
    }
  }


  // Verify that the body is correct JSON with an "event" element
  if (!(*body).IsObject() ||
      !(*body).HasMember("event") ||
      !(*body)["event"].IsObject())
  {
    LOG_WARNING("JSON document was either not valid or did not have an 'event' key");
    delete body;
    return HTTP_BAD_RESULT;
  }

  // Verify the Role-Of-Node and Node-Functionality AVPs are present (we use these
  // to distinguish devices in path for the same SIP call ID.
  if ((!(*body)["event"].HasMember("Service-Information")) ||
      (!(*body)["event"]["Service-Information"].IsObject()) ||
      (!(*body)["event"]["Service-Information"].HasMember("IMS-Information")) ||
      (!(*body)["event"]["Service-Information"]["IMS-Information"].IsObject()))
  {
    LOG_ERROR("IMS-Information not included in the event description");
    delete body;
    return HTTP_BAD_RESULT;
  }
  else
  {

    rapidjson::Value& ims_information_json = (*body)["event"]["Service-Information"]["IMS-Information"];
    rapidjson::Value::MemberIterator role_of_node_json = ims_information_json.FindMember("Role-Of-Node");
    if ((role_of_node_json == ims_information_json.MemberEnd()) || !(role_of_node_json->value.IsInt()))
    {
      LOG_ERROR("No Role-Of-Node in IMS-Information");
      delete body;
      return HTTP_BAD_RESULT;
    }

    role_of_node = (role_of_node_t)role_of_node_json->value.GetInt();

    rapidjson::Value::MemberIterator node_function_json = ims_information_json.FindMember("Node-Functionality");
    if ((node_function_json == ims_information_json.MemberEnd()) || !(node_function_json->value.IsInt()))
    {
      LOG_ERROR("No Node-Functionality in IMS-Information");
      delete body;
      return HTTP_BAD_RESULT;
    }

    node_functionality = (node_functionality_t)node_function_json->value.GetInt();
  }

  // Verify that there is an Accounting-Record-Type and it is one of
  // the four valid types
  if (!((*body)["event"].HasMember("Accounting-Record-Type") &&
        ((*body)["event"]["Accounting-Record-Type"].IsInt())))
  {
    LOG_WARNING("Accounting-Record-Type not available in JSON");
    delete body;
    return HTTP_BAD_RESULT;
  }

  Rf::AccountingRecordType record_type((*body)["event"]["Accounting-Record-Type"].GetInt());
  if (!record_type.isValid())
  {
    LOG_ERROR("Accounting-Record-Type was not one of START/INTERIM/STOP/EVENT");
    delete body;
    return HTTP_BAD_RESULT;
  }

  // Get the Acct-Interim-Interval if present
  if ((*body)["event"].HasMember("Acct-Interim-Interval") &&
        ((*body)["event"]["Acct-Interim-Interval"].IsInt()))
  {
    session_refresh_time = (*body)["event"]["Acct-Interim-Interval"].GetInt();
  }

  // If we have a START or EVENT Accounting-Record-Type, we must have
  // a list of CCFs to use as peers.
  // If these are missing, Ralf can't send the ACR onto a CDF, but it has
  // successfully processed the request. Log this and return 200 OK with
  // no further processing.
  if (record_type.isStart() || record_type.isEvent())
  {
    if (!((body->HasMember("peers")) && (*body)["peers"].IsObject()))
    {
      LOG_ERROR("JSON lacked a 'peers' object (mandatory for START/EVENT)");
      SAS::Event missing_peers(trail, SASEvent::INCOMING_REQUEST_NO_PEERS, 0);
      missing_peers.add_static_param(record_type.code());
      SAS::report_event(missing_peers);

      delete body;
      return HTTP_OK;
    }

    if (!((*body)["peers"].HasMember("ccf")) ||(!(*body)["peers"]["ccf"].IsArray()) || ((*body)["peers"]["ccf"].Size() == 0))
    {
      LOG_ERROR("JSON lacked a 'ccf' array, or the array was empty (mandatory for START/EVENT)");
      delete body;
      return HTTP_BAD_RESULT;
    }

    for (rapidjson::SizeType i = 0; i < (*body)["peers"]["ccf"].Size(); i++)
    {
      if (!(*body)["peers"]["ccf"][i].IsString())
      {
        LOG_ERROR("JSON contains a 'ccf' array but not all the elements are strings");
        delete body;
        return HTTP_BAD_RESULT;
      }
      LOG_DEBUG("Adding CCF %s", (*body)["peers"]["ccf"][i].GetString());
      ccfs.push_back((*body)["peers"]["ccf"][i].GetString());
    }
  }

  SAS::Event incoming(trail, SASEvent::INCOMING_REQUEST, 0);
  incoming.add_static_param(record_type.code());
  incoming.add_static_param(node_functionality);
  SAS::report_event(incoming);

  *msg = new Message(call_id,
                     role_of_node,
                     node_functionality,
                     body,
                     record_type,
                     session_refresh_time,
                     trail,
                     timer_interim);
  if (!ccfs.empty())
  {
    (*msg)->ccfs = ccfs;
  }

  return HTTP_OK;
}
