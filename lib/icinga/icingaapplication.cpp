/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2015 Icinga Development Team (http://www.icinga.org)    *
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

#include "icinga/icingaapplication.hpp"
#include "icinga/icingaapplication.tcpp"
#include "icinga/cib.hpp"
#include "config/configwriter.hpp"
#include "config/configcompiler.hpp"
#include "base/configtype.hpp"
#include "base/logger.hpp"
#include "base/objectlock.hpp"
#include "base/convert.hpp"
#include "base/debug.hpp"
#include "base/utility.hpp"
#include "base/timer.hpp"
#include "base/scriptglobal.hpp"
#include "base/initialize.hpp"
#include "base/statsfunction.hpp"

using namespace icinga;

static Timer::Ptr l_RetentionTimer;

REGISTER_TYPE(IcingaApplication);
INITIALIZE_ONCE(&IcingaApplication::StaticInitialize);

void IcingaApplication::StaticInitialize(void)
{
	ScriptGlobal::Set("EnableNotifications", true);
	ScriptGlobal::Set("EnableEventHandlers", true);
	ScriptGlobal::Set("EnableFlapping", true);
	ScriptGlobal::Set("EnableHostChecks", true);
	ScriptGlobal::Set("EnableServiceChecks", true);
	ScriptGlobal::Set("EnablePerfdata", true);

	String node_name = Utility::GetFQDN();

	if (node_name.IsEmpty()) {
		Log(LogNotice, "IcingaApplication", "No FQDN available. Trying Hostname.");
		node_name = Utility::GetHostName();

		if (node_name.IsEmpty()) {
			Log(LogWarning, "IcingaApplication", "No FQDN nor Hostname available. Setting Nodename to 'localhost'.");
			node_name = "localhost";
		}
	}

	ScriptGlobal::Set("NodeName", node_name);
}

REGISTER_STATSFUNCTION(IcingaApplicationStats, &IcingaApplication::StatsFunc);

void IcingaApplication::StatsFunc(const Dictionary::Ptr& status, const Array::Ptr& perfdata)
{
	Dictionary::Ptr nodes = new Dictionary();

	BOOST_FOREACH(const IcingaApplication::Ptr& icingaapplication, ConfigType::GetObjectsByType<IcingaApplication>()) {
		Dictionary::Ptr stats = new Dictionary();
		stats->Set("node_name", icingaapplication->GetNodeName());
		stats->Set("enable_notifications", icingaapplication->GetEnableNotifications());
		stats->Set("enable_event_handlers", icingaapplication->GetEnableEventHandlers());
		stats->Set("enable_flapping", icingaapplication->GetEnableFlapping());
		stats->Set("enable_host_checks", icingaapplication->GetEnableHostChecks());
		stats->Set("enable_service_checks", icingaapplication->GetEnableServiceChecks());
		stats->Set("enable_perfdata", icingaapplication->GetEnablePerfdata());
		stats->Set("pid", Utility::GetPid());
		stats->Set("program_start", Application::GetStartTime());
		stats->Set("version", Application::GetVersion());

		nodes->Set(icingaapplication->GetName(), stats);
	}

	status->Set("icingaapplication", nodes);
}

/**
 * The entry point for the Icinga application.
 *
 * @returns An exit status.
 */
int IcingaApplication::Main(void)
{
	Log(LogDebug, "IcingaApplication", "In IcingaApplication::Main()");

	/* periodically dump the program state */
	l_RetentionTimer = new Timer();
	l_RetentionTimer->SetInterval(300);
	l_RetentionTimer->OnTimerExpired.connect(boost::bind(&IcingaApplication::DumpProgramState, this));
	l_RetentionTimer->Start();

	/* restore modified attributes */
	if (Utility::PathExists(GetModAttrPath())) {
		Expression *expression = ConfigCompiler::CompileFile(GetModAttrPath());

		if (expression) {
			try {
				ScriptFrame frame;
				expression->Evaluate(frame);
			} catch (const std::exception& ex) {
				Log(LogCritical, "config", DiagnosticInformation(ex));
			}
		}

		delete expression;
	}

	RunEventLoop();

	Log(LogInformation, "IcingaApplication", "Icinga has shut down.");

	return EXIT_SUCCESS;
}

void IcingaApplication::OnShutdown(void)
{
	ASSERT(!OwnsLock());

	{
		ObjectLock olock(this);
		l_RetentionTimer->Stop();
	}

	DumpProgramState();
}

static void PersistModAttrHelper(const ConfigWriter::Ptr& cw, ConfigObject::Ptr& previousObject, const ConfigObject::Ptr& object, const String& attr, const Value& value)
{
	if (object != previousObject) {
		if (previousObject)
			cw->EmitRaw("}\n\n");

		cw->EmitRaw("var obj = ");

		Array::Ptr args1 = new Array();
		args1->Add(object->GetReflectionType()->GetName());
		args1->Add(object->GetName());
		cw->EmitFunctionCall("get_object", args1);

		cw->EmitRaw("\nif (obj) {\n");
	}

	cw->EmitRaw("\tobj.");

	Array::Ptr args2 = new Array();
	args2->Add(attr);
	args2->Add(value);
	cw->EmitFunctionCall("modify_attribute", args2);

	cw->EmitRaw("\n");

	previousObject = object;
}

void IcingaApplication::DumpProgramState(void)
{
	ConfigObject::DumpObjects(GetStatePath());

	ConfigWriter::Ptr cw = new ConfigWriter(GetModAttrPath());
	ConfigObject::Ptr previousObject;
	ConfigObject::DumpModifiedAttributes(boost::bind(&PersistModAttrHelper, cw, boost::ref(previousObject), _1, _2, _3));

	if (previousObject)
		cw->EmitRaw("\n}\n");
}

IcingaApplication::Ptr IcingaApplication::GetInstance(void)
{
	return static_pointer_cast<IcingaApplication>(Application::GetInstance());
}

Dictionary::Ptr IcingaApplication::GetVars(void) const
{
	return ScriptGlobal::Get("Vars", &Empty);
}

String IcingaApplication::GetNodeName(void) const
{
	return ScriptGlobal::Get("NodeName");
}

bool IcingaApplication::ResolveMacro(const String& macro, const CheckResult::Ptr&, Value *result) const
{
	double now = Utility::GetTime();

	if (macro == "timet") {
		*result = Convert::ToString((long)now);
		return true;
	} else if (macro == "long_date_time") {
		*result = Utility::FormatDateTime("%Y-%m-%d %H:%M:%S %z", now);
		return true;
	} else if (macro == "short_date_time") {
		*result = Utility::FormatDateTime("%Y-%m-%d %H:%M:%S", now);
		return true;
	} else if (macro == "date") {
		*result = Utility::FormatDateTime("%Y-%m-%d", now);
		return true;
	} else if (macro == "time") {
		*result = Utility::FormatDateTime("%H:%M:%S %z", now);
		return true;
	} else if (macro == "uptime") {
		*result = Utility::FormatDuration(Utility::GetTime() - Application::GetStartTime());
		return true;
	}

	Dictionary::Ptr vars = GetVars();

	if (vars && vars->Contains(macro)) {
		*result = vars->Get(macro);
		return true;
	}

	if (macro.Contains("num_services")) {
		ServiceStatistics ss = CIB::CalculateServiceStats();

		if (macro == "num_services_ok") {
			*result = Convert::ToString(ss.services_ok);
			return true;
		} else if (macro == "num_services_warning") {
			*result = Convert::ToString(ss.services_warning);
			return true;
		} else if (macro == "num_services_critical") {
			*result = Convert::ToString(ss.services_critical);
			return true;
		} else if (macro == "num_services_unknown") {
			*result = Convert::ToString(ss.services_unknown);
			return true;
		} else if (macro == "num_services_pending") {
			*result = Convert::ToString(ss.services_pending);
			return true;
		} else if (macro == "num_services_unreachable") {
			*result = Convert::ToString(ss.services_unreachable);
			return true;
		} else if (macro == "num_services_flapping") {
			*result = Convert::ToString(ss.services_flapping);
			return true;
		} else if (macro == "num_services_in_downtime") {
			*result = Convert::ToString(ss.services_in_downtime);
			return true;
		} else if (macro == "num_services_acknowledged") {
			*result = Convert::ToString(ss.services_acknowledged);
			return true;
		}
	}
	else if (macro.Contains("num_hosts")) {
		HostStatistics hs = CIB::CalculateHostStats();

		if (macro == "num_hosts_up") {
			*result = Convert::ToString(hs.hosts_up);
			return true;
		} else if (macro == "num_hosts_down") {
			*result = Convert::ToString(hs.hosts_down);
			return true;
		} else if (macro == "num_hosts_unreachable") {
			*result = Convert::ToString(hs.hosts_unreachable);
			return true;
		} else if (macro == "num_hosts_flapping") {
			*result = Convert::ToString(hs.hosts_flapping);
			return true;
		} else if (macro == "num_hosts_in_downtime") {
			*result = Convert::ToString(hs.hosts_in_downtime);
			return true;
		} else if (macro == "num_hosts_acknowledged") {
			*result = Convert::ToString(hs.hosts_acknowledged);
			return true;
		}
	}

	return false;
}

bool IcingaApplication::GetEnableNotifications(void) const
{
	return ScriptGlobal::Get("EnableNotifications");
}

void IcingaApplication::SetEnableNotifications(bool enabled)
{
	ScriptGlobal::Set("EnableNotifications", enabled);
}

bool IcingaApplication::GetEnableEventHandlers(void) const
{
	return ScriptGlobal::Get("EnableEventHandlers");
}

void IcingaApplication::SetEnableEventHandlers(bool enabled)
{
	ScriptGlobal::Set("EnableEventHandlers", enabled);
}

bool IcingaApplication::GetEnableFlapping(void) const
{
	return ScriptGlobal::Get("EnableFlapping");
}

void IcingaApplication::SetEnableFlapping(bool enabled)
{
	ScriptGlobal::Set("EnableFlapping", enabled);
}

bool IcingaApplication::GetEnableHostChecks(void) const
{
	return ScriptGlobal::Get("EnableHostChecks");
}

void IcingaApplication::SetEnableHostChecks(bool enabled)
{
	ScriptGlobal::Set("EnableHostChecks", enabled);
}

bool IcingaApplication::GetEnableServiceChecks(void) const
{
	return ScriptGlobal::Get("EnableServiceChecks");
}

void IcingaApplication::SetEnableServiceChecks(bool enabled)
{
	ScriptGlobal::Set("EnableServiceChecks", enabled);
}

bool IcingaApplication::GetEnablePerfdata(void) const
{
	return ScriptGlobal::Get("EnablePerfdata");
}

void IcingaApplication::SetEnablePerfdata(bool enabled)
{
	ScriptGlobal::Set("EnablePerfdata", enabled);
}
