// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusDefines.h"
#include "VaCuusSystemInterface.h"
#include "VaCuusTestLogCapture.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ONE UE_LOG PER LINE OF AN RmlUi MESSAGE (bead VaCuus-w87.1). RmlUi hands some diagnostics
 * over as one multi-line string -- the data interpreter's program dump is the one every
 * failed data expression produces (DataExpression.cpp:930-931) -- and a single UE_LOG of it
 * writes the timestamp, the category and the verbosity on the FIRST line only. Every later
 * line reaches the log file bare, so a filter on "LogVaCuus: Warning" or on "[Rml]" misses
 * it, which is what the field report's "1 'A' 1" line was.
 *
 * Called directly: LogMessage is a pure forwarder with no RmlUi state behind it, so the
 * interface needs no UI thread and no Rml::Initialise to be exercised.
 *
 * RESTORE-THE-BUG: put back the single UE_LOG per case in FVaCuusSystemInterface::LogMessage
 * and the capture sees ONE entry carrying both lines -- the entry count reads 1 and the
 * "no entry spans a newline" assertion goes red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusRmlMultiLineLogTest, "VaCuus.Log.RmlMultiLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusRmlMultiLineLogTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("[Rml] VaCuusMultiLineProbe first"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("[Rml] VaCuusMultiLineProbe second"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FVaCuusTestLogCapture Capture;

	FVaCuusSystemInterface SystemInterface;
	SystemInterface.LogMessage(Rml::Log::LT_WARNING, "VaCuusMultiLineProbe first\nVaCuusMultiLineProbe second\n");

	int32 NumProbeEntries = 0;
	bool bAnySpansNewline = false;
	for (const FVaCuusTestLogCapture::FEntry& Entry : Capture.Snapshot())
	{
		if (!Entry.Text.Contains(TEXT("VaCuusMultiLineProbe"), ESearchCase::CaseSensitive))
		{
			continue;
		}

		++NumProbeEntries;
		bAnySpansNewline |= Entry.Text.Contains(TEXT("\n"));
		TestTrue(FString::Printf(TEXT("'%s' carries the [Rml] prefix"), *Entry.Text),
			Entry.Text.StartsWith(TEXT("[Rml] "), ESearchCase::CaseSensitive));
		TestEqual(FString::Printf(TEXT("'%s' is in LogVaCuus"), *Entry.Text), Entry.Category, LogVaCuus.GetCategoryName());

		// NOT Entry.Verbosity: the framework rewrites a line that matches an expectation to
		// Verbose before any device sees it (AutomationTest.cpp:314-320), so the capture reads
		// Verbose here. That each line left at Warning is what the two expectations above
		// assert -- they match at "Warning or higher", one line each.
	}

	TestEqual(TEXT("two lines in, two log entries out (the trailing newline adds no empty third)"), NumProbeEntries, 2);
	TestFalse(TEXT("no entry spans a newline"), bAnySpansNewline);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
