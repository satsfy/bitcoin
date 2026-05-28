// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <clientversion.h>
#include <unordered_set>

#include <rpc/util.h>
#include <shutdown.h>
#include <sync.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/signals2/signal.hpp>

#include <cassert>
#include <memory> // for unique_ptr
#include <mutex>
#include <unordered_map>

static Mutex g_rpc_warmup_mutex;
static std::atomic<bool> g_rpc_running{false};
static bool fRPCInWarmup GUARDED_BY(g_rpc_warmup_mutex) = true;
static std::string rpcWarmupStatus GUARDED_BY(g_rpc_warmup_mutex) = "RPC server started";
/* Timer-creating functions */
static RPCTimerInterface* timerInterface = nullptr;
/* Map of name to timer. */
static Mutex g_deadline_timers_mutex;
static std::map<std::string, std::unique_ptr<RPCTimerBase> > deadlineTimers GUARDED_BY(g_deadline_timers_mutex);
static bool ExecuteCommand(const CRPCCommand& command, const JSONRPCRequest& request, UniValue& result, bool last_handler);

struct RPCCommandExecutionInfo
{
    std::string method;
    int64_t start;
};

struct RPCServerInfo
{
    Mutex mutex;
    std::list<RPCCommandExecutionInfo> active_commands GUARDED_BY(mutex);
};

static RPCServerInfo g_rpc_server_info;

struct RPCCommandExecution
{
    std::list<RPCCommandExecutionInfo>::iterator it;
    explicit RPCCommandExecution(const std::string& method)
    {
        LOCK(g_rpc_server_info.mutex);
        it = g_rpc_server_info.active_commands.insert(g_rpc_server_info.active_commands.end(), {method, GetTimeMicros()});
    }
    ~RPCCommandExecution()
    {
        LOCK(g_rpc_server_info.mutex);
        g_rpc_server_info.active_commands.erase(it);
    }
};

static struct CRPCSignals
{
    boost::signals2::signal<void ()> Started;
    boost::signals2::signal<void ()> Stopped;
} g_rpcSignals;

void RPCServer::OnStarted(std::function<void ()> slot)
{
    g_rpcSignals.Started.connect(slot);
}

void RPCServer::OnStopped(std::function<void ()> slot)
{
    g_rpcSignals.Stopped.connect(slot);
}

std::string CRPCTable::help(const std::string& strCommand, const JSONRPCRequest& helpreq) const
{
    std::string strRet;
    std::string category;
    std::set<intptr_t> setDone;
    std::vector<std::pair<std::string, const CRPCCommand*> > vCommands;

    for (const auto& entry : mapCommands)
        vCommands.push_back(make_pair(entry.second.front()->category + entry.first, entry.second.front()));
    sort(vCommands.begin(), vCommands.end());

    JSONRPCRequest jreq(helpreq);
    jreq.fHelp = true;
    jreq.params = UniValue();

    for (const std::pair<std::string, const CRPCCommand*>& command : vCommands)
    {
        const CRPCCommand *pcmd = command.second;
        std::string strMethod = pcmd->name;
        if ((strCommand != "" || pcmd->category == "hidden") && strMethod != strCommand)
            continue;
        jreq.strMethod = strMethod;
        try
        {
            UniValue unused_result;
            if (setDone.insert(pcmd->unique_id).second)
                pcmd->actor(jreq, unused_result, true /* last_handler */);
        }
        catch (const std::exception& e)
        {
            // Help text is returned in an exception
            std::string strHelp = std::string(e.what());
            if (strCommand == "")
            {
                if (strHelp.find('\n') != std::string::npos)
                    strHelp = strHelp.substr(0, strHelp.find('\n'));

                if (category != pcmd->category)
                {
                    if (!category.empty())
                        strRet += "\n";
                    category = pcmd->category;
                    strRet += "== " + Capitalize(category) + " ==\n";
                }
            }
            strRet += strHelp + "\n";
        }
    }
    if (strRet == "")
        strRet = strprintf("help: unknown command: %s\n", strCommand);
    strRet = strRet.substr(0,strRet.size()-1);
    return strRet;
}

static RPCHelpMan help()
{
    return RPCHelpMan{"help",
                "\nList all commands, or get help for a specified command.\n",
                {
                    {"command", RPCArg::Type::STR, /* default */ "all commands", "The command to get help on"},
                },
                RPCResult{
                    RPCResult::Type::STR, "", "The help text"
                },
                RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& jsonRequest) -> UniValue
{
    std::string strCommand;
    if (jsonRequest.params.size() > 0)
        strCommand = jsonRequest.params[0].get_str();

    return tableRPC.help(strCommand, jsonRequest);
},
    };
}

static RPCHelpMan stop()
{
    static const std::string RESULT{PACKAGE_NAME " stopping"};
    return RPCHelpMan{"stop",
    // Also accept the hidden 'wait' integer argument (milliseconds)
    // For instance, 'stop 1000' makes the call wait 1 second before returning
    // to the client (intended for testing)
                "\nRequest a graceful shutdown of " PACKAGE_NAME ".",
                {
                    {"wait", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "how long to wait in ms", "", {}, /* hidden */ true},
                },
                RPCResult{RPCResult::Type::STR, "", "A string with the content '" + RESULT + "'"},
                RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& jsonRequest) -> UniValue
{
    // Event loop will exit after current HTTP requests have been handled, so
    // this reply will get back to the client.
    StartShutdown();
    if (jsonRequest.params[0].isNum()) {
        UninterruptibleSleep(std::chrono::milliseconds{jsonRequest.params[0].get_int()});
    }
    return RESULT;
},
    };
}

static RPCHelpMan uptime()
{
    return RPCHelpMan{"uptime",
                "\nReturns the total uptime of the server.\n",
                            {},
                            RPCResult{
                                RPCResult::Type::NUM, "", "The number of seconds that the server has been running"
                            },
                RPCExamples{
                    HelpExampleCli("uptime", "")
                + HelpExampleRpc("uptime", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return GetTime() - GetStartupTime();
}
    };
}

static RPCHelpMan getrpcinfo()
{
    return RPCHelpMan{"getrpcinfo",
                "\nReturns details of the RPC server.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::ARR, "active_commands", "All active commands",
                        {
                            {RPCResult::Type::OBJ, "", "Information about an active command",
                            {
                                 {RPCResult::Type::STR, "method", "The name of the RPC command"},
                                 {RPCResult::Type::NUM, "duration", "The running time in microseconds"},
                            }},
                        }},
                        {RPCResult::Type::STR, "logpath", "The complete file path to the debug log"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getrpcinfo", "")
                + HelpExampleRpc("getrpcinfo", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    LOCK(g_rpc_server_info.mutex);
    UniValue active_commands(UniValue::VARR);
    for (const RPCCommandExecutionInfo& info : g_rpc_server_info.active_commands) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("method", info.method);
        entry.pushKV("duration", GetTimeMicros() - info.start);
        active_commands.push_back(entry);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("active_commands", active_commands);

    const std::string path = LogInstance().m_file_path.string();
    UniValue log_path(UniValue::VSTR, path);
    result.pushKV("logpath", log_path);

    return result;
}
    };
}

// clang-format off
namespace {
UniValue OpenRPCArgSchema(const RPCArg& arg);
UniValue OpenRPCResultSchema(const RPCResult& result);

UniValue MakeObject(std::initializer_list<std::pair<std::string, UniValue>> entries)
{
    UniValue obj{UniValue::VOBJ};
    for (const auto& [key, value] : entries) {
        obj.pushKV(key, value);
    }
    return obj;
}

void PushUniqueSchema(UniValue& schemas, std::unordered_set<std::string>& seen, UniValue schema)
{
    const std::string serialized{schema.write()};
    if (seen.insert(serialized).second) schemas.push_back(std::move(schema));
}

// NOLINTNEXTLINE(misc-no-recursion)
UniValue DedupArrayItemsSchema(const std::vector<RPCArg>& inner)
{
    if (inner.empty()) return UniValue{UniValue::VOBJ};
    if (inner.size() == 1) return OpenRPCArgSchema(inner.front());

    UniValue one_of{UniValue::VARR};
    std::unordered_set<std::string> seen;
    for (const auto& item : inner) {
        PushUniqueSchema(one_of, seen, OpenRPCArgSchema(item));
    }

    if (one_of.size() == 1) return one_of[0];

    UniValue items{UniValue::VOBJ};
    items.pushKV("oneOf", std::move(one_of));
    return items;
}

// NOLINTNEXTLINE(misc-no-recursion)
UniValue DedupArrayItemsSchema(const std::vector<RPCResult>& inner)
{
    if (inner.empty()) return UniValue{UniValue::VOBJ};
    if (inner.size() == 1) return OpenRPCResultSchema(inner.front());

    UniValue one_of{UniValue::VARR};
    std::unordered_set<std::string> seen;
    for (const auto& item : inner) {
        PushUniqueSchema(one_of, seen, OpenRPCResultSchema(item));
    }

    if (one_of.size() == 1) return one_of[0];

    UniValue items{UniValue::VOBJ};
    items.pushKV("oneOf", std::move(one_of));
    return items;
}

// NOLINTNEXTLINE(misc-no-recursion)
UniValue OpenRPCArgSchema(const RPCArg& arg)
{
    switch (arg.m_type) {
    case RPCArg::Type::STR:
        return MakeObject({{"type", "string"}});
    case RPCArg::Type::STR_HEX:
        return MakeObject({{"type", "string"}, {"pattern", "^[0-9a-fA-F]+$"}});
    case RPCArg::Type::NUM:
        return MakeObject({{"type", "number"}});
    case RPCArg::Type::BOOL:
        return MakeObject({{"type", "boolean"}});
    case RPCArg::Type::AMOUNT: {
        UniValue one_of{UniValue::VARR};
        one_of.push_back(MakeObject({{"type", "number"}}));
        one_of.push_back(MakeObject({{"type", "string"}}));
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("oneOf", std::move(one_of));
        return schema;
    }
    case RPCArg::Type::RANGE: {
        UniValue prefix_items{UniValue::VARR};
        prefix_items.push_back(MakeObject({{"type", "number"}}));
        prefix_items.push_back(MakeObject({{"type", "number"}}));
        UniValue range_schema{UniValue::VOBJ};
        range_schema.pushKV("type", "array");
        range_schema.pushKV("prefixItems", std::move(prefix_items));
        range_schema.pushKV("minItems", 2);
        range_schema.pushKV("maxItems", 2);
        UniValue one_of{UniValue::VARR};
        one_of.push_back(MakeObject({{"type", "number"}}));
        one_of.push_back(std::move(range_schema));
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("oneOf", std::move(one_of));
        return schema;
    }
    case RPCArg::Type::ARR: {
        UniValue items{DedupArrayItemsSchema(arg.m_inner)};
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("type", "array");
        schema.pushKV("items", std::move(items));
        return schema;
    }
    case RPCArg::Type::OBJ: {
        UniValue properties{UniValue::VOBJ};
        UniValue required{UniValue::VARR};
        for (const auto& inner : arg.m_inner) {
            if (inner.m_hidden) continue;
            UniValue prop{OpenRPCArgSchema(inner)};
            if (!inner.m_description.empty()) prop.pushKV("description", inner.m_description);
            properties.pushKV(inner.GetFirstName(), std::move(prop));
            if (!inner.IsOptional()) required.push_back(inner.GetFirstName());
        }
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("type", "object");
        schema.pushKV("properties", std::move(properties));
        schema.pushKV("additionalProperties", false);
        if (!required.empty()) schema.pushKV("required", std::move(required));
        return schema;
    }
    case RPCArg::Type::OBJ_USER_KEYS: {
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("type", "object");
        if (!arg.m_inner.empty()) {
            schema.pushKV("additionalProperties", OpenRPCArgSchema(arg.m_inner[0]));
        } else {
            schema.pushKV("additionalProperties", true);
        }
        return schema;
    }
    } // no default case, so the compiler can warn about missing cases
    CHECK_NONFATAL(false);
}

// NOLINTNEXTLINE(misc-no-recursion)
UniValue OpenRPCResultSchema(const RPCResult& result)
{
    switch (result.m_type) {
    case RPCResult::Type::STR:
    case RPCResult::Type::STR_AMOUNT:
        return MakeObject({{"type", "string"}});
    case RPCResult::Type::STR_HEX:
        return MakeObject({{"type", "string"}, {"pattern", "^[0-9a-fA-F]+$"}});
    case RPCResult::Type::NUM:
        return MakeObject({{"type", "number"}});
    case RPCResult::Type::NUM_TIME: {
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("type", "number");
        schema.pushKV("x-bitcoin-unit", "unix-time");
        return schema;
    }
    case RPCResult::Type::BOOL:
        return MakeObject({{"type", "boolean"}});
    case RPCResult::Type::NONE:
        return MakeObject({{"type", "null"}});
    case RPCResult::Type::ARR: {
        UniValue items{DedupArrayItemsSchema(result.m_inner)};
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("type", "array");
        schema.pushKV("items", std::move(items));
        return schema;
    }
    case RPCResult::Type::ARR_FIXED: {
        UniValue prefix_items{UniValue::VARR};
        for (const auto& inner : result.m_inner) {
            prefix_items.push_back(OpenRPCResultSchema(inner));
        }
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("type", "array");
        schema.pushKV("prefixItems", std::move(prefix_items));
        schema.pushKV("minItems", uint64_t(result.m_inner.size()));
        schema.pushKV("maxItems", uint64_t(result.m_inner.size()));
        return schema;
    }
    case RPCResult::Type::OBJ: {
        UniValue properties{UniValue::VOBJ};
        UniValue required{UniValue::VARR};
        bool has_elision{false};
        for (const auto& inner : result.m_inner) {
            if (inner.m_type == RPCResult::Type::ELISION) {
                has_elision = true;
                continue;
            }
            if (inner.m_key_name.empty()) continue;
            UniValue prop{OpenRPCResultSchema(inner)};
            if (!inner.m_description.empty()) prop.pushKV("description", inner.m_description);
            properties.pushKV(inner.m_key_name, std::move(prop));
            if (!inner.m_optional) required.push_back(inner.m_key_name);
        }
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("type", "object");
        schema.pushKV("properties", std::move(properties));
        schema.pushKV("additionalProperties", has_elision);
        if (!required.empty()) schema.pushKV("required", std::move(required));
        return schema;
    }
    case RPCResult::Type::OBJ_DYN: {
        UniValue schema{UniValue::VOBJ};
        schema.pushKV("type", "object");
        if (!result.m_inner.empty()) {
            schema.pushKV("additionalProperties", OpenRPCResultSchema(result.m_inner[0]));
        } else {
            schema.pushKV("additionalProperties", UniValue{UniValue::VOBJ});
        }
        return schema;
    }
    case RPCResult::Type::ELISION:
        return UniValue{UniValue::VOBJ};
    } // no default case, so the compiler can warn about missing cases
    CHECK_NONFATAL(false);
}
} // namespace

static RPCHelpMan getopenrpcinfo()
{
    return RPCHelpMan{
        "getopenrpcinfo",
        "Returns an OpenRPC document for currently available RPC commands.\n",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "openrpc", "OpenRPC specification version."},
                {RPCResult::Type::OBJ, "info", "Metadata about this JSON-RPC interface.",
                    {
                        {RPCResult::Type::STR, "title", "API title."},
                        {RPCResult::Type::STR, "version", "Bitcoin Core version string."},
                        {RPCResult::Type::STR, "description", "API description."},
                    }},
                {RPCResult::Type::ARR, "methods", "Documented RPC methods.",
                    {{RPCResult::Type::OBJ, "", "An RPC method description object.",
                        {
                            {RPCResult::Type::STR, "name", "Method name."},
                            {RPCResult::Type::STR, "description", "Method description."},
                            {RPCResult::Type::ARR, "params", "Method parameters.",
                                {{RPCResult::Type::OBJ, "", "A parameter.",
                                    {
                                        {RPCResult::Type::STR, "name", "Parameter name."},
                                        {RPCResult::Type::BOOL, "required", "Whether the parameter is required."},
                                        {RPCResult::Type::STR, "schema", /*optional=*/true, "JSON Schema for the parameter."},
                                        {RPCResult::Type::STR, "description", /*optional=*/true, "Parameter description."},
                                        {RPCResult::Type::ARR, "x-bitcoin-aliases", /*optional=*/true, "Alternative parameter names.",
                                            {{RPCResult::Type::STR, "", "An alias."}}},
                                        {RPCResult::Type::BOOL, "x-bitcoin-placeholder", /*optional=*/true, "Whether the parameter is retained only for compatibility."},
                                        {RPCResult::Type::BOOL, "x-bitcoin-also-positional", /*optional=*/true, "Whether the parameter can also be passed positionally."},
                                    }}}},
                            {RPCResult::Type::OBJ, "result", "Method result.",
                                {
                                    {RPCResult::Type::STR, "name", "Result name."},
                                    {RPCResult::Type::STR, "schema", /*optional=*/true, "JSON Schema for the result."},
                                }},
                            {RPCResult::Type::STR, "x-bitcoin-category", "RPC category."},
                        }}}},
            },
            },
        RPCExamples{
            HelpExampleCli("getopenrpcinfo", "")
            + HelpExampleRpc("getopenrpcinfo", "")
        },
        [](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return tableRPC.buildOpenRPCDoc();
},
    };
}

static const CRPCCommand vRPCCommands[] =
{ //  category              name                      actor (function)         argNames
  //  --------------------- ------------------------  -----------------------  ----------
    /* Overall control/query calls */
    { "control",            "getopenrpcinfo",             &getopenrpcinfo,             {}  },
    { "control",            "getrpcinfo",             &getrpcinfo,             {}  },
    { "control",            "help",                   &help,                   {"command"}  },
    { "control",            "stop",                   &stop,                   {"wait"}  },
    { "control",            "uptime",                 &uptime,                 {}  },
};
// clang-format on

CRPCTable::CRPCTable()
{
    for (const auto& c : vRPCCommands) {
        appendCommand(c.name, &c);
    }
}

void CRPCTable::appendCommand(const std::string& name, const CRPCCommand* pcmd)
{
    CHECK_NONFATAL(!IsRPCRunning()); // Only add commands before rpc is running

    mapCommands[name].push_back(pcmd);
}

bool CRPCTable::removeCommand(const std::string& name, const CRPCCommand* pcmd)
{
    auto it = mapCommands.find(name);
    if (it != mapCommands.end()) {
        auto new_end = std::remove(it->second.begin(), it->second.end(), pcmd);
        if (it->second.end() != new_end) {
            it->second.erase(new_end, it->second.end());
            return true;
        }
    }
    return false;
}

void StartRPC()
{
    LogPrint(BCLog::RPC, "Starting RPC\n");
    g_rpc_running = true;
    g_rpcSignals.Started();
}

void InterruptRPC()
{
    static std::once_flag g_rpc_interrupt_flag;
    // This function could be called twice if the GUI has been started with -server=1.
    std::call_once(g_rpc_interrupt_flag, []() {
        LogPrint(BCLog::RPC, "Interrupting RPC\n");
        // Interrupt e.g. running longpolls
        g_rpc_running = false;
    });
}

void StopRPC()
{
    static std::once_flag g_rpc_stop_flag;
    // This function could be called twice if the GUI has been started with -server=1.
    assert(!g_rpc_running);
    std::call_once(g_rpc_stop_flag, []() {
        LogPrint(BCLog::RPC, "Stopping RPC\n");
        WITH_LOCK(g_deadline_timers_mutex, deadlineTimers.clear());
        DeleteAuthCookie();
        g_rpcSignals.Stopped();
    });
}

bool IsRPCRunning()
{
    return g_rpc_running;
}

void RpcInterruptionPoint()
{
    if (!IsRPCRunning()) throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
}

void SetRPCWarmupStatus(const std::string& newStatus)
{
    LOCK(g_rpc_warmup_mutex);
    rpcWarmupStatus = newStatus;
}

void SetRPCWarmupFinished()
{
    LOCK(g_rpc_warmup_mutex);
    assert(fRPCInWarmup);
    fRPCInWarmup = false;
}

bool RPCIsInWarmup(std::string *outStatus)
{
    LOCK(g_rpc_warmup_mutex);
    if (outStatus)
        *outStatus = rpcWarmupStatus;
    return fRPCInWarmup;
}

bool IsDeprecatedRPCEnabled(const std::string& method)
{
    const std::vector<std::string> enabled_methods = gArgs.GetArgs("-deprecatedrpc");

    return find(enabled_methods.begin(), enabled_methods.end(), method) != enabled_methods.end();
}

static UniValue JSONRPCExecOne(JSONRPCRequest jreq, const UniValue& req)
{
    UniValue rpc_result(UniValue::VOBJ);

    try {
        jreq.parse(req);

        UniValue result = tableRPC.execute(jreq);
        rpc_result = JSONRPCReplyObj(result, NullUniValue, jreq.id);
    }
    catch (const UniValue& objError)
    {
        rpc_result = JSONRPCReplyObj(NullUniValue, objError, jreq.id);
    }
    catch (const std::exception& e)
    {
        rpc_result = JSONRPCReplyObj(NullUniValue,
                                     JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }

    return rpc_result;
}

std::string JSONRPCExecBatch(const JSONRPCRequest& jreq, const UniValue& vReq)
{
    UniValue ret(UniValue::VARR);
    for (unsigned int reqIdx = 0; reqIdx < vReq.size(); reqIdx++)
        ret.push_back(JSONRPCExecOne(jreq, vReq[reqIdx]));

    return ret.write() + "\n";
}

/**
 * Process named arguments into a vector of positional arguments, based on the
 * passed-in specification for the RPC call's arguments.
 */
static inline JSONRPCRequest transformNamedArguments(const JSONRPCRequest& in, const std::vector<std::string>& argNames)
{
    JSONRPCRequest out = in;
    out.params = UniValue(UniValue::VARR);
    // Build a map of parameters, and remove ones that have been processed, so that we can throw a focused error if
    // there is an unknown one.
    const std::vector<std::string>& keys = in.params.getKeys();
    const std::vector<UniValue>& values = in.params.getValues();
    std::unordered_map<std::string, const UniValue*> argsIn;
    for (size_t i=0; i<keys.size(); ++i) {
        argsIn[keys[i]] = &values[i];
    }
    // Process expected parameters.
    int hole = 0;
    for (const std::string &argNamePattern: argNames) {
        std::vector<std::string> vargNames;
        boost::algorithm::split(vargNames, argNamePattern, boost::algorithm::is_any_of("|"));
        auto fr = argsIn.end();
        for (const std::string & argName : vargNames) {
            fr = argsIn.find(argName);
            if (fr != argsIn.end()) {
                break;
            }
        }
        if (fr != argsIn.end()) {
            for (int i = 0; i < hole; ++i) {
                // Fill hole between specified parameters with JSON nulls,
                // but not at the end (for backwards compatibility with calls
                // that act based on number of specified parameters).
                out.params.push_back(UniValue());
            }
            hole = 0;
            out.params.push_back(*fr->second);
            argsIn.erase(fr);
        } else {
            hole += 1;
        }
    }
    // If there are still arguments in the argsIn map, this is an error.
    if (!argsIn.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown named parameter " + argsIn.begin()->first);
    }
    // Return request with named arguments transformed to positional arguments
    return out;
}

UniValue CRPCTable::execute(const JSONRPCRequest &request) const
{
    // Return immediately if in warmup
    {
        LOCK(g_rpc_warmup_mutex);
        if (fRPCInWarmup)
            throw JSONRPCError(RPC_IN_WARMUP, rpcWarmupStatus);
    }

    // Find method
    auto it = mapCommands.find(request.strMethod);
    if (it != mapCommands.end()) {
        UniValue result;
        for (const auto& command : it->second) {
            if (ExecuteCommand(*command, request, result, &command == &it->second.back())) {
                return result;
            }
        }
    }
    throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");
}

static bool ExecuteCommand(const CRPCCommand& command, const JSONRPCRequest& request, UniValue& result, bool last_handler)
{
    try
    {
        RPCCommandExecution execution(request.strMethod);
        // Execute, convert arguments to array if necessary
        if (request.params.isObject()) {
            return command.actor(transformNamedArguments(request, command.argNames), result, last_handler);
        } else {
            return command.actor(request, result, last_handler);
        }
    }
    catch (const std::exception& e)
    {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
}

UniValue CRPCTable::buildOpenRPCDoc() const
{
    std::vector<std::string> method_names;
    for (const auto& [name, cmds] : mapCommands) {
        if (cmds.empty()) continue;
        const CRPCCommand* cmd{cmds.front()};
        if (cmd->category == "hidden" || !cmd->metadata_fn) continue;
        method_names.push_back(name);
    }
    std::sort(method_names.begin(), method_names.end());

    UniValue methods{UniValue::VARR};
    for (const auto& method_name : method_names) {
        const CRPCCommand* cmd{mapCommands.at(method_name).front()};
        RPCHelpMan helpman{cmd->metadata_fn()};

        UniValue params{UniValue::VARR};
        for (const auto& arg : helpman.GetArgs()) {
            if (arg.m_hidden) continue;
            UniValue param{UniValue::VOBJ};
            param.pushKV("name", arg.GetFirstName());
            param.pushKV("required", !arg.IsOptional());
            param.pushKV("schema", OpenRPCArgSchema(arg));

            std::vector<std::string> names;
            { std::string s_ = arg.m_names; size_t p_ = 0, n_;
              while ((n_ = s_.find('|', p_)) != std::string::npos) { names.push_back(s_.substr(p_, n_-p_)); p_ = n_+1; }
              names.push_back(s_.substr(p_)); }
            if (names.size() > 1) {
                UniValue aliases{UniValue::VARR};
                for (size_t i{1}; i < names.size(); ++i) aliases.push_back(names[i]);
                param.pushKV("x-bitcoin-aliases", std::move(aliases));
            }
            if (!arg.m_description.empty()) param.pushKV("description", arg.m_description);
            params.push_back(std::move(param));
        }

        UniValue result_schema{UniValue::VOBJ};
        const auto& results{helpman.GetResults().m_results};
        if (results.size() == 1) {
            result_schema = OpenRPCResultSchema(results[0]);
        } else if (results.size() > 1) {
            UniValue one_of{UniValue::VARR};
            for (const auto& r : results) {
                UniValue schema{OpenRPCResultSchema(r)};
                if (!r.m_cond.empty()) schema.pushKV("description", r.m_cond);
                one_of.push_back(std::move(schema));
            }
            if (one_of.size() == 1) {
                result_schema = one_of[0];
            } else if (one_of.size() > 1) {
                result_schema.pushKV("oneOf", std::move(one_of));
            }
        }

        UniValue method{UniValue::VOBJ};
        method.pushKV("name", method_name);
        method.pushKV("description", TrimString(helpman.GetDescription()));
        method.pushKV("params", std::move(params));
        UniValue result{UniValue::VOBJ};
        result.pushKV("name", "result");
        result.pushKV("schema", std::move(result_schema));
        method.pushKV("result", std::move(result));
        method.pushKV("x-bitcoin-category", cmd->category);
        methods.push_back(std::move(method));
    }

    std::string version{FormatFullVersion()};

    UniValue info{UniValue::VOBJ};
    info.pushKV("title", "Bitcoin Core JSON-RPC");
    info.pushKV("version", version);
    info.pushKV("description", "Autogenerated from Bitcoin Core RPC metadata.");

    UniValue doc{UniValue::VOBJ};
    doc.pushKV("openrpc", "1.3.2");
    doc.pushKV("info", std::move(info));
    doc.pushKV("methods", std::move(methods));
    return doc;
}

std::vector<std::string> CRPCTable::listCommands() const
{
    std::vector<std::string> commandList;
    for (const auto& i : mapCommands) commandList.emplace_back(i.first);
    return commandList;
}

void RPCSetTimerInterfaceIfUnset(RPCTimerInterface *iface)
{
    if (!timerInterface)
        timerInterface = iface;
}

void RPCSetTimerInterface(RPCTimerInterface *iface)
{
    timerInterface = iface;
}

void RPCUnsetTimerInterface(RPCTimerInterface *iface)
{
    if (timerInterface == iface)
        timerInterface = nullptr;
}

void RPCRunLater(const std::string& name, std::function<void()> func, int64_t nSeconds)
{
    if (!timerInterface)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No timer handler registered for RPC");
    LOCK(g_deadline_timers_mutex);
    deadlineTimers.erase(name);
    LogPrint(BCLog::RPC, "queue run of timer %s in %i seconds (using %s)\n", name, nSeconds, timerInterface->Name());
    deadlineTimers.emplace(name, std::unique_ptr<RPCTimerBase>(timerInterface->NewTimer(func, nSeconds*1000)));
}

int RPCSerializationFlags()
{
    int flag = 0;
    if (gArgs.GetArg("-rpcserialversion", DEFAULT_RPC_SERIALIZE_VERSION) == 0)
        flag |= SERIALIZE_TRANSACTION_NO_WITNESS;
    return flag;
}

CRPCTable tableRPC;
