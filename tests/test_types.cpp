// UnLeaf Unit Tests - types.h utility functions
// Tests: ToLower, IsValidProcessName, IsCriticalProcess, GetCriticalProcesses

#include <gtest/gtest.h>
#include "common/types.h"

using namespace unleaf;

// ============================================================
// ToLowerTest
// ============================================================

TEST(ToLowerTest, EmptyString) {
    EXPECT_EQ(ToLower(L""), L"");
}

TEST(ToLowerTest, AllLowercase) {
    EXPECT_EQ(ToLower(L"notepad.exe"), L"notepad.exe");
}

TEST(ToLowerTest, AllUppercase) {
    EXPECT_EQ(ToLower(L"NOTEPAD.EXE"), L"notepad.exe");
}

TEST(ToLowerTest, MixedCase) {
    EXPECT_EQ(ToLower(L"NotePad.Exe"), L"notepad.exe");
}

TEST(ToLowerTest, NonAlphabetic) {
    EXPECT_EQ(ToLower(L"app_123-v2.exe"), L"app_123-v2.exe");
}

// ============================================================
// IsValidProcessNameTest
// ============================================================

TEST(IsValidProcessNameTest, ValidSimple) {
    EXPECT_TRUE(IsValidProcessName(L"notepad.exe"));
}

TEST(IsValidProcessNameTest, ValidUnderscore) {
    EXPECT_TRUE(IsValidProcessName(L"my_app.exe"));
}

TEST(IsValidProcessNameTest, ValidHyphen) {
    EXPECT_TRUE(IsValidProcessName(L"app-test.exe"));
}

TEST(IsValidProcessNameTest, ValidDot) {
    EXPECT_TRUE(IsValidProcessName(L"app.v1.exe"));
}

TEST(IsValidProcessNameTest, EmptyString) {
    EXPECT_FALSE(IsValidProcessName(L""));
}

TEST(IsValidProcessNameTest, TooLong) {
    std::wstring name(257, L'a');
    name += L".exe";
    EXPECT_FALSE(IsValidProcessName(name));
}

TEST(IsValidProcessNameTest, NoExeExtension) {
    EXPECT_FALSE(IsValidProcessName(L"notepad.txt"));
}

TEST(IsValidProcessNameTest, NoExtension) {
    EXPECT_FALSE(IsValidProcessName(L"notepad"));
}

TEST(IsValidProcessNameTest, PathTraversal) {
    EXPECT_FALSE(IsValidProcessName(L"..\\notepad.exe"));
}

TEST(IsValidProcessNameTest, AbsolutePathDrive) {
    EXPECT_FALSE(IsValidProcessName(L"C:\\notepad.exe"));
}

TEST(IsValidProcessNameTest, AbsolutePathUNC) {
    EXPECT_FALSE(IsValidProcessName(L"\\\\server\\notepad.exe"));
}

TEST(IsValidProcessNameTest, ForwardSlash) {
    EXPECT_FALSE(IsValidProcessName(L"dir/notepad.exe"));
}

TEST(IsValidProcessNameTest, Backslash) {
    EXPECT_FALSE(IsValidProcessName(L"dir\\notepad.exe"));
}

TEST(IsValidProcessNameTest, InvalidCharSpace) {
    EXPECT_FALSE(IsValidProcessName(L"note pad.exe"));
}

TEST(IsValidProcessNameTest, InvalidCharAt) {
    EXPECT_FALSE(IsValidProcessName(L"note@pad.exe"));
}

TEST(IsValidProcessNameTest, CaseInsensitiveExe) {
    EXPECT_TRUE(IsValidProcessName(L"notepad.EXE"));
}

TEST(IsValidProcessNameTest, MinimumLength) {
    EXPECT_TRUE(IsValidProcessName(L"a.exe"));
}

TEST(IsValidProcessNameTest, DotExeOnly) {
    // ".exe" passes current validation (length==4, valid chars, ends with .exe)
    // Edge case: no name part before extension, but accepted by implementation
    EXPECT_TRUE(IsValidProcessName(L".exe"));
}

TEST(IsValidProcessNameTest, TooShort) {
    EXPECT_FALSE(IsValidProcessName(L"exe"));
    EXPECT_FALSE(IsValidProcessName(L"ab"));
}

// ============================================================
// IsCriticalProcessTest
// ============================================================

TEST(IsCriticalProcessTest, KnownCriticalCsrss) {
    EXPECT_TRUE(IsCriticalProcess(L"csrss.exe"));
}

TEST(IsCriticalProcessTest, KnownCriticalLsass) {
    EXPECT_TRUE(IsCriticalProcess(L"lsass.exe"));
}

TEST(IsCriticalProcessTest, KnownCriticalSvchost) {
    EXPECT_TRUE(IsCriticalProcess(L"svchost.exe"));
}

TEST(IsCriticalProcessTest, CaseInsensitive) {
    EXPECT_TRUE(IsCriticalProcess(L"CSRSS.EXE"));
}

TEST(IsCriticalProcessTest, NotCritical) {
    EXPECT_FALSE(IsCriticalProcess(L"notepad.exe"));
}

TEST(IsCriticalProcessTest, UnleafServiceSelf) {
    EXPECT_TRUE(IsCriticalProcess(L"unleaf_service.exe"));
}

TEST(IsCriticalProcessTest, UnleafManagerSelf) {
    EXPECT_TRUE(IsCriticalProcess(L"unleaf_manager.exe"));
}

TEST(IsCriticalProcessTest, KnownCriticalFontdrvhost) {
    EXPECT_TRUE(IsCriticalProcess(L"fontdrvhost.exe"));
    EXPECT_TRUE(IsCriticalProcess(L"FONTDRVHOST.EXE"));
}

TEST(IsCriticalProcessTest, KnownCriticalAudiodg) {
    EXPECT_TRUE(IsCriticalProcess(L"audiodg.exe"));
    EXPECT_TRUE(IsCriticalProcess(L"AudioDg.exe"));
}

// ============================================================
// GetCriticalProcessesTest
// ============================================================

TEST(GetCriticalProcessesTest, ReturnsNonEmpty) {
    const auto& procs = GetCriticalProcesses();
    EXPECT_FALSE(procs.empty());
}

TEST(GetCriticalProcessesTest, ContainsExpected) {
    const auto& procs = GetCriticalProcesses();
    EXPECT_TRUE(procs.count(L"csrss.exe") > 0);
    EXPECT_TRUE(procs.count(L"lsass.exe") > 0);
    EXPECT_TRUE(procs.count(L"svchost.exe") > 0);
    EXPECT_TRUE(procs.count(L"explorer.exe") > 0);
    EXPECT_TRUE(procs.count(L"fontdrvhost.exe") > 0);
    EXPECT_TRUE(procs.count(L"audiodg.exe") > 0);
    EXPECT_TRUE(procs.count(L"conhost.exe") > 0);
    EXPECT_TRUE(procs.count(L"securityhealthservice.exe") > 0);
    EXPECT_TRUE(procs.count(L"msmpeng.exe") > 0);
}

TEST(GetCriticalProcessesTest, ExpectedCount) {
    EXPECT_EQ(GetCriticalProcesses().size(), 18u);
}

TEST(GetCriticalProcessesTest, ReturnsSameReference) {
    const auto& ref1 = GetCriticalProcesses();
    const auto& ref2 = GetCriticalProcesses();
    EXPECT_EQ(&ref1, &ref2);
}

// ============================================================
// NormalizePathTest
// ============================================================

TEST(NormalizePathTest, EmptyString) {
    EXPECT_EQ(NormalizePath(L""), L"");
}

TEST(NormalizePathTest, StripExtendedPrefix) {
    EXPECT_EQ(NormalizePath(L"\\\\?\\C:\\foo\\bar.exe"), L"c:\\foo\\bar.exe");
}

TEST(NormalizePathTest, StripNtDevicePrefix) {
    EXPECT_EQ(NormalizePath(L"\\??\\C:\\foo\\bar.exe"), L"c:\\foo\\bar.exe");
}

TEST(NormalizePathTest, StripUNCPrefix) {
    EXPECT_EQ(NormalizePath(L"\\\\?\\UNC\\server\\share\\foo.exe"), L"\\\\server\\share\\foo.exe");
}

TEST(NormalizePathTest, ForwardSlashToBackslash) {
    EXPECT_EQ(NormalizePath(L"C:/foo/bar.exe"), L"c:\\foo\\bar.exe");
}

TEST(NormalizePathTest, TrailingBackslashRemoved) {
    EXPECT_EQ(NormalizePath(L"C:\\foo\\bar\\"), L"c:\\foo\\bar");
}

TEST(NormalizePathTest, Lowercase) {
    EXPECT_EQ(NormalizePath(L"C:\\FOO\\BAR.EXE"), L"c:\\foo\\bar.exe");
}

TEST(NormalizePathTest, PlainPathUnchangedExceptCase) {
    EXPECT_EQ(NormalizePath(L"C:\\Windows\\System32\\notepad.exe"), L"c:\\windows\\system32\\notepad.exe");
}

// ============================================================
// IsPathEntryTest
// ============================================================

TEST(IsPathEntryTest, DriveAbsolutePath) {
    EXPECT_TRUE(IsPathEntry(L"C:\\foo\\game.exe"));
}

TEST(IsPathEntryTest, UNCPath) {
    EXPECT_TRUE(IsPathEntry(L"\\\\server\\share\\game.exe"));
}

TEST(IsPathEntryTest, RelativePathWithBackslash) {
    EXPECT_TRUE(IsPathEntry(L"foo\\bar.exe"));
}

TEST(IsPathEntryTest, ForwardSlashPath) {
    EXPECT_TRUE(IsPathEntry(L"foo/bar.exe"));
}

TEST(IsPathEntryTest, NameOnly) {
    EXPECT_FALSE(IsPathEntry(L"game.exe"));
}

TEST(IsPathEntryTest, EmptyString) {
    EXPECT_FALSE(IsPathEntry(L""));
}

// ============================================================
// ExtractFileNameTest
// ============================================================

TEST(ExtractFileNameTest, WithBackslash) {
    EXPECT_EQ(ExtractFileName(L"c:\\path\\to\\game.exe"), L"game.exe");
}

TEST(ExtractFileNameTest, WithForwardSlash) {
    EXPECT_EQ(ExtractFileName(L"c:/path/to/game.exe"), L"game.exe");
}

TEST(ExtractFileNameTest, NameOnly) {
    EXPECT_EQ(ExtractFileName(L"game.exe"), L"game.exe");
}

TEST(ExtractFileNameTest, TrailingBackslash) {
    EXPECT_EQ(ExtractFileName(L"c:\\path\\"), L"");
}

TEST(ExtractFileNameTest, EmptyString) {
    EXPECT_EQ(ExtractFileName(L""), L"");
}

// ============================================================
// IsValidTargetEntryTest
// ============================================================

TEST(IsValidTargetEntryTest, ValidNameOnly) {
    EXPECT_TRUE(IsValidTargetEntry(L"chrome.exe"));
}

TEST(IsValidTargetEntryTest, ValidNameOnlyUpperExt) {
    EXPECT_TRUE(IsValidTargetEntry(L"chrome.EXE"));
}

TEST(IsValidTargetEntryTest, ValidAbsolutePathDrive) {
    EXPECT_TRUE(IsValidTargetEntry(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"));
}

TEST(IsValidTargetEntryTest, ValidAbsolutePathWithSpaces) {
    EXPECT_TRUE(IsValidTargetEntry(L"C:\\Program Files (x86)\\game\\game.exe"));
}

TEST(IsValidTargetEntryTest, RelativePathRejected) {
    EXPECT_FALSE(IsValidTargetEntry(L"foo\\bar.exe"));
}

TEST(IsValidTargetEntryTest, RelativePathForwardSlashRejected) {
    EXPECT_FALSE(IsValidTargetEntry(L"foo/bar.exe"));
}

TEST(IsValidTargetEntryTest, DotDotSegmentRejected) {
    EXPECT_FALSE(IsValidTargetEntry(L"C:\\foo\\..\\bar.exe"));
}

TEST(IsValidTargetEntryTest, NoExeExtensionPath) {
    EXPECT_FALSE(IsValidTargetEntry(L"C:\\foo\\bar.txt"));
}

TEST(IsValidTargetEntryTest, EmptyString) {
    EXPECT_FALSE(IsValidTargetEntry(L""));
}

// ============================================================
// IsCanonicalPathImplTest
// ============================================================

TEST(IsCanonicalPathImplTest, ValidDrivePath) {
    EXPECT_TRUE(IsCanonicalPathImpl(L"c:\\program files\\chrome.exe"));
}

TEST(IsCanonicalPathImplTest, ValidUNCPath) {
    EXPECT_TRUE(IsCanonicalPathImpl(L"\\\\server\\share\\app.exe"));
}

TEST(IsCanonicalPathImplTest, EmptyString) {
    EXPECT_FALSE(IsCanonicalPathImpl(L""));
}

TEST(IsCanonicalPathImplTest, HasUppercase) {
    EXPECT_FALSE(IsCanonicalPathImpl(L"C:\\foo\\bar.exe"));
}

TEST(IsCanonicalPathImplTest, HasForwardSlash) {
    EXPECT_FALSE(IsCanonicalPathImpl(L"c:/foo/bar.exe"));
}

TEST(IsCanonicalPathImplTest, HasDotDot) {
    EXPECT_FALSE(IsCanonicalPathImpl(L"c:\\foo\\..\\bar.exe"));
}

TEST(IsCanonicalPathImplTest, HasExtendedPrefix) {
    EXPECT_FALSE(IsCanonicalPathImpl(L"\\\\?\\c:\\foo\\bar.exe"));
}

TEST(IsCanonicalPathImplTest, HasNtDevicePrefix) {
    EXPECT_FALSE(IsCanonicalPathImpl(L"\\??\\c:\\foo\\bar.exe"));
}

TEST(IsCanonicalPathImplTest, IncompleteUNC_NoShare) {
    EXPECT_FALSE(IsCanonicalPathImpl(L"\\\\server\\"));
}

TEST(IsCanonicalPathImplTest, IncompleteUNC_NoServer) {
    EXPECT_FALSE(IsCanonicalPathImpl(L"\\\\\\share"));
}

TEST(IsCanonicalPathImplTest, UNC_EmptyShareName) {
    EXPECT_FALSE(IsCanonicalPathImpl(L"\\\\server\\\\file.exe"));
}

TEST(IsCanonicalPathImplTest, RelativePath) {
    EXPECT_FALSE(IsCanonicalPathImpl(L"foo\\bar.exe"));
}
