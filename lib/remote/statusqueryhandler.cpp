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

#include "remote/statusqueryhandler.hpp"
#include "remote/httputility.hpp"
#include "remote/filterutility.hpp"
#include "base/serializer.hpp"
#include "base/dependencygraph.hpp"
#include "base/configtype.hpp"
#include <boost/algorithm/string.hpp>
#include <set>

using namespace icinga;

REGISTER_URLHANDLER("/v1", StatusQueryHandler);

bool StatusQueryHandler::HandleRequest(const ApiUser::Ptr& user, HttpRequest& request, HttpResponse& response)
{
	if (request.RequestMethod != "GET")
		return false;

	if (request.RequestUrl->GetPath().size() < 2)
		return false;

	Type::Ptr type = FilterUtility::TypeFromPluralName(request.RequestUrl->GetPath()[1]);

	if (!type)
		return false;

	QueryDescription qd;
	qd.Types.insert(type);

	std::vector<String> joinTypes;
	joinTypes.push_back(type->GetName());

	Dictionary::Ptr params = HttpUtility::FetchRequestParameters(request);

	params->Set("type", type->GetName());

	if (request.RequestUrl->GetPath().size() >= 3) {
		String attr = type->GetName();
		boost::algorithm::to_lower(attr);
		params->Set(attr, request.RequestUrl->GetPath()[2]);
	}

	std::vector<ConfigObject::Ptr> objs = FilterUtility::GetFilterTargets(qd, params);

	Array::Ptr results = new Array();

	std::set<String> attrs;
	Array::Ptr uattrs = params->Get("attrs");

	if (uattrs) {
		ObjectLock olock(uattrs);
		BOOST_FOREACH(const String& uattr, uattrs) {
			attrs.insert(uattr);
		}
	}

	BOOST_FOREACH(const ConfigObject::Ptr& obj, objs) {
		Dictionary::Ptr result1 = new Dictionary();
		results->Add(result1);

		Dictionary::Ptr resultAttrs = new Dictionary();
		result1->Set("attrs", resultAttrs);

		BOOST_FOREACH(const String& joinType, joinTypes) {
			String prefix = joinType;
			boost::algorithm::to_lower(prefix);

			for (int fid = 0; fid < type->GetFieldCount(); fid++) {
				Field field = type->GetFieldInfo(fid);
				String aname = prefix + "." + field.Name;
				if (!attrs.empty() && attrs.find(aname) == attrs.end())
					continue;

				Value val = static_cast<Object::Ptr>(obj)->GetField(fid);
				Value sval = Serialize(val, FAConfig | FAState);
				resultAttrs->Set(aname, sval);
			}
		}

		Array::Ptr used_by = new Array();
		result1->Set("used_by", used_by);

		BOOST_FOREACH(const Object::Ptr& pobj, DependencyGraph::GetParents((obj))) {
			ConfigObject::Ptr configObj = dynamic_pointer_cast<ConfigObject>(pobj);

			if (!configObj)
				continue;

			Dictionary::Ptr refInfo = new Dictionary();
			refInfo->Set("type", configObj->GetType()->GetName());
			refInfo->Set("name", configObj->GetName());
			used_by->Add(refInfo);
		}
	}

	Dictionary::Ptr result = new Dictionary();
	result->Set("results", results);

	response.SetStatus(200, "OK");
	HttpUtility::SendJsonBody(response, result);

	return true;
}

