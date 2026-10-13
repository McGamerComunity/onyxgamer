#pragma once
// Shared constants for the service <-> agent control channel.
// Messages are short ASCII strings on a message-mode named pipe:
//   agent -> service: "HELLO <session> <pid>" (first message), then "HB" (heartbeat)
//   service -> agent: "ACK" (after HELLO), then "OK" or "STOP" (reply to each HB)
// The service never trusts numbers sent by the agent. Identity comes from the pipe
// (GetNamedPipeClientProcessId / GetNamedPipeClientSessionId).
namespace onyx {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\OnyxControl";
inline constexpr wchar_t kServiceName[] = L"OnyxStream";
inline constexpr wchar_t kServiceExe[] = L"onyx_service.exe";
inline constexpr wchar_t kAgentExe[] = L"onyx_agent.exe";

inline constexpr unsigned kHeartbeatIntervalMs = 2000;
inline constexpr unsigned kHeartbeatTimeoutMs = 10000;
inline constexpr unsigned kHelloTimeoutMs = 15000;

}  // namespace onyx
