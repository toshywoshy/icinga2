/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2016 Icinga Development Team (https://www.icinga.org/)  *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "perfdata/logstashwriter.hpp"
#include "perfdata/logstashwriter.tcpp"
#include "icinga/service.hpp"
#include "icinga/macroprocessor.hpp"
#include "icinga/icingaapplication.hpp"
#include "icinga/compatutility.hpp"
#include "icinga/perfdatavalue.hpp"
#include "base/tcpsocket.hpp"
#include "base/configtype.hpp"
#include "base/objectlock.hpp"
#include "base/logger.hpp"
#include "base/convert.hpp"
#include "base/utility.hpp"
#include "base/application.hpp"
#include "base/stream.hpp"
#include "base/networkstream.hpp"
#include "base/exception.hpp"
#include "base/statsfunction.hpp"
#include "base/json.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/replace.hpp>


using namespace icinga;

REGISTER_TYPE(LogstashWriter);

REGISTER_STATSFUNCTION(LogstashWriter, &LogstashWriter::StatsFunc);

void LogstashWriter::StatsFunc(const Dictionary::Ptr& status, const Array::Ptr&)
{
        Dictionary::Ptr nodes = new Dictionary();

        BOOST_FOREACH(const LogstashWriter::Ptr& logstashwriter, ConfigType::GetObjectsByType<LogstashWriter>()) {
                nodes->Set(logstashwriter->GetName(), 1); //add more stats
        }

        status->Set("logstashwriter", nodes);
}

void LogstashWriter::Start(bool runtimeCreated)
{
        ObjectImpl<LogstashWriter>::Start(runtimeCreated);

        m_ReconnectTimer = new Timer();
        m_ReconnectTimer->SetInterval(10);
        m_ReconnectTimer->OnTimerExpired.connect(boost::bind(&LogstashWriter::ReconnectTimerHandler, this));
        m_ReconnectTimer->Start();
        m_ReconnectTimer->Reschedule(0);

        Service::OnNewCheckResult.connect(boost::bind(&LogstashWriter::CheckResultHandler, this, _1, _2));
}

void LogstashWriter::ReconnectTimerHandler(void)
{
        if (m_Stream)
                return;

        TcpSocket::Ptr socket = new TcpSocket();

        Log(LogNotice, "LogstashWriter")
            << "Reconnecting to Logstash on host '" << GetHost() << "' port '" << GetPort() << "'.";

        try {
                socket->Connect(GetHost(), GetPort());
        } catch (std::exception&) {
                Log(LogCritical, "LogstashWriter")
                    << "Can't connect to Logstash on host '" << GetHost() << "' port '" << GetPort() << "'.";
                return;
        }

        m_Stream = new NetworkStream(socket);
}

/*void LogstashWriter::metrikToSend(const String& prefix, const String& name, double value, double ts, const Checkable::Ptr& checkable, const CheckResult::Ptr& cr)
{                       // const String&,               const String&,                  double,                                                                 double
        SendMetric(prefix_metadata,             "current_attempt",              checkable->GetCheckAttempt(),                   ts);
        SendMetric(prefix_metadata,             "max_check_attempts",   checkable->GetMaxCheckAttempts(),               ts);
        SendMetric(prefix_metadata,             "state_type",                   checkable->GetStateType(),                              ts);
        SendMetric(prefix_metadata,             "reachable",                    checkable->IsReachable(),                               ts);
        SendMetric(prefix_metadata,             "downtime_depth",               checkable->GetDowntimeDepth(),                  ts);
        SendMetric(prefix_metadata,             "latency",                              Service::CalculateLatency(cr),                  ts);
        SendMetric(prefix_metadata,             "execution_time",               Service::CalculateExecutionTime(cr),    ts);
} */


void LogstashWriter::CheckResultHandler(const Checkable::Ptr& checkable, const CheckResult::Ptr& cr)
{
        CONTEXT("Processing check result for '" + checkable->GetName() + "'");
        if (!IcingaApplication::GetInstance()->GetEnablePerfdata() || !checkable->GetEnablePerfdata())
                return;
        /***********************************************************************************************/

        String prefix;
        double ts = cr->GetExecutionEnd();
        Service::Ptr service = dynamic_pointer_cast<Service>(checkable);
        Host::Ptr host;
        MacroProcessor::ResolverList resolvers;

        if (service)
        {       host = service->GetHost();
                resolvers.push_back(std::make_pair("service", service));
        }else host = static_pointer_cast<Host>(checkable);

        resolvers.push_back(std::make_pair("host", host));
        resolvers.push_back(std::make_pair("icinga", IcingaApplication::GetInstance()));


        /* new mode below. old mode in else tree with 2.4, deprecate it in 2.6 */
        if (!GetEnableLegacyMode())
        {       String prefix_perfdata = prefix + ".perfdata";
                String prefix_metadata = prefix + ".metadata";

                (service) ? prefix = MacroProcessor::ResolveMacros(GetServiceNameTemplate(), resolvers, cr, NULL, boost::bind(&LogstashWriter::EscapeMacroMetric, _1, false))
                                  : prefix = MacroProcessor::ResolveMacros(GetHostNameTemplate(), resolvers, cr, NULL, boost::bind(&LogstashWriter::EscapeMacroMetric, _1, false));

                if (GetEnableSendMetadata())
                {       (service) ? SendMetric(prefix_metadata, "state", service->GetState(), ts)
                                          :     SendMetric(prefix_metadata, "state", host->GetState(), ts);

                        SendMetric(prefix_metadata, "current_attempt", checkable->GetCheckAttempt(), ts);               //with metadata
                        SendMetric(prefix_metadata, "max_check_attempts", checkable->GetMaxCheckAttempts(), ts);
                        SendMetric(prefix_metadata, "state_type", checkable->GetStateType(), ts);
                        SendMetric(prefix_metadata, "reachable", checkable->IsReachable(), ts);
                        SendMetric(prefix_metadata, "downtime_depth", checkable->GetDowntimeDepth(), ts);
                        SendMetric(prefix_metadata, "latency", cr->CalculateLatency(), ts);
                        SendMetric(prefix_metadata, "execution_time", cr->CalculateExecutionTime(), ts);
                }
                SendPerfdata(prefix_perfdata, cr, ts);

        }else{

                if (service)
                {       prefix = MacroProcessor::ResolveMacros(GetServiceNameTemplate(), resolvers, cr, NULL, boost::bind(&LogstashWriter::EscapeMacroMetric, _1, true));
                        SendMetric(prefix, "state", service->GetState(), ts);
                }else{
                        prefix = MacroProcessor::ResolveMacros(GetHostNameTemplate(), resolvers, cr, NULL, boost::bind(&LogstashWriter::EscapeMacroMetric, _1, true));
                        SendMetric(prefix, "state", host->GetState(), ts);
                }
                SendMetric(prefix, "current_attempt", checkable->GetCheckAttempt(), ts);                                        // just prefix
                SendMetric(prefix, "max_check_attempts", checkable->GetMaxCheckAttempts(), ts);
                SendMetric(prefix, "state_type", checkable->GetStateType(), ts);
                SendMetric(prefix, "reachable", checkable->IsReachable(), ts);
                SendMetric(prefix, "downtime_depth", checkable->GetDowntimeDepth(), ts);
                SendMetric(prefix, "latency", cr->CalculateLatency(), ts);
                SendMetric(prefix, "execution_time", cr->CalculateExecutionTime(), ts);
                SendPerfdata(prefix, cr, ts);
        }
}

void LogstashWriter::SendPerfdata(const String& prefix, const CheckResult::Ptr& cr, double ts)
{
        Array::Ptr perfdata = cr->GetPerformanceData();

        if (!perfdata) return;

        ObjectLock olock(perfdata);
        BOOST_FOREACH(const Value& val, perfdata) {
                PerfdataValue::Ptr pdv;

                if (val.IsObjectType<PerfdataValue>())
                        pdv = val;
                else {
                        try {
                                pdv = PerfdataValue::Parse(val);
                        } catch (const std::exception&) {
                                Log(LogWarning, "LogstashWriter")
                                    << "Ignoring invalid perfdata value: " << val;
                                continue;
                        }
                }

                /* new mode below. old mode in else tree with 2.4, deprecate it in 2.6 */
                if (!GetEnableLegacyMode()) {
                        String escaped_key = EscapeMetricLabel(pdv->GetLabel());

                        SendMetric(prefix, escaped_key + ".value", pdv->GetValue(), ts);

                        if (GetEnableSendThresholds()) {
                                if (pdv->GetCrit())
                                        SendMetric(prefix, escaped_key + ".crit", pdv->GetCrit(), ts);
                                if (pdv->GetWarn())
                                        SendMetric(prefix, escaped_key + ".warn", pdv->GetWarn(), ts);
                                if (pdv->GetMin())
                                        SendMetric(prefix, escaped_key + ".min", pdv->GetMin(), ts);
                                if (pdv->GetMax())
                                        SendMetric(prefix, escaped_key + ".max", pdv->GetMax(), ts);
                        }
                } else {
                        String escaped_key = EscapeMetric(pdv->GetLabel());
                        boost::algorithm::replace_all(escaped_key, "::", ".");
                        SendMetric(prefix, escaped_key, pdv->GetValue(), ts);

                        if (pdv->GetCrit())
                                SendMetric(prefix, escaped_key + "_crit", pdv->GetCrit(), ts);
                        if (pdv->GetWarn())
                                SendMetric(prefix, escaped_key + "_warn", pdv->GetWarn(), ts);
                        if (pdv->GetMin())
                                SendMetric(prefix, escaped_key + "_min", pdv->GetMin(), ts);
                        if (pdv->GetMax())
                                SendMetric(prefix, escaped_key + "_max", pdv->GetMax(), ts);
                }
        }
}

void LogstashWriter::SendMetric(const String& prefix, const String& name, double value, double ts)
{
        std::ostringstream msgbuf;
        msgbuf << prefix << "." << name << " " << Convert::ToString(value) << " " << static_cast<long>(ts);

        Log(LogDebug, "LogstashWriter") << "Add to metric list:'" << msgbuf.str() << "'.";

        // do not send \n to debug log
        msgbuf << "\n";
        String metric = JsonEncode(&msgbuf.str()[0] /*, &msgbuf[0].GetLength()*/ );
        ObjectLock olock(this);

        if (!m_Stream) return;

        try {
                m_Stream->Write(metric.CStr(), metric.GetLength());
        } catch (const std::exception& ex) {
                Log(LogCritical, "LogstashWriter") << "Cannot write to TCP socket on host '" << GetHost() << "' port '" << GetPort() << "'.";
                m_Stream.reset();
        }
}

String LogstashWriter::EscapeMetric(const String& str, bool legacyMode)
{
        String result = str;

        //don't allow '.' in metric prefixes
        boost::replace_all(result, " ", "_");
        boost::replace_all(result, ".", "_");
        boost::replace_all(result, "\\", "_");
        boost::replace_all(result, "/", "_");

        if (legacyMode)
                boost::replace_all(result, "-", "_");

        return result;
}

String LogstashWriter::EscapeMetricLabel(const String& str)
{
        String result = str;

        //allow to pass '.' in perfdata labels
        boost::replace_all(result, " ", "_");
        boost::replace_all(result, "\\", "_");
        boost::replace_all(result, "/", "_");
        boost::replace_all(result, "::", ".");

        return result;
}

Value LogstashWriter::EscapeMacroMetric(const Value& value, bool legacyMode)
{
        if (value.IsObjectType<Array>()) {
                Array::Ptr arr = value;
                Array::Ptr result = new Array();

                ObjectLock olock(arr);
                BOOST_FOREACH(const Value& arg, arr) {
                        result->Add(EscapeMetric(arg, legacyMode));
                }

                return Utility::Join(result, '.');
        } else
                return EscapeMetric(value, legacyMode);
}

void LogstashWriter::ValidateHostNameTemplate(const String& value, const ValidationUtils& utils)
{
        ObjectImpl<LogstashWriter>::ValidateHostNameTemplate(value, utils);

        if (!MacroProcessor::ValidateMacroString(value))
                BOOST_THROW_EXCEPTION(ValidationError(this, boost::assign::list_of("host_name_template"), "Closing $ not found in macro format string '" + value + "'."));
}

void LogstashWriter::ValidateServiceNameTemplate(const String& value, const ValidationUtils& utils)
{
        ObjectImpl<LogstashWriter>::ValidateServiceNameTemplate(value, utils);

        if (!MacroProcessor::ValidateMacroString(value))
                BOOST_THROW_EXCEPTION(ValidationError(this, boost::assign::list_of("service_name_template"), "Closing $ not found in macro format string '" + value + "'."));
}

