#pragma once
// UnLeaf - Unhandled exception crash dump writer
//
// Installs a SetUnhandledExceptionFilter handler that writes a minidump
// (MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory) to
// <baseDir>\crash\UnLeaf_Service_<yyyymmdd_hhmmss.sss>.dmp and then
// chains to the default handler so WER still records the event.
//
// Design notes:
// - Dumps are written next to the executable (not %ProgramData%) to
//   preserve UnLeaf's portable philosophy of keeping all generated
//   artefacts inside the install folder.
// - Disabled by default. Enabled via UnLeaf.ini: [Logging] CrashDump=1.
// - Install is idempotent; call sites should check IsCrashDumpEnabled()
//   on UnLeafConfig before invoking.

#include <string>

namespace unleaf {

// Install the process-wide unhandled exception filter. Idempotent.
// Creates <baseDir>\crash\ if it does not exist.
// Caller is expected to have verified that crash dumping is enabled
// via configuration before calling.
void InstallCrashHandler(const std::wstring& baseDir);

} // namespace unleaf
