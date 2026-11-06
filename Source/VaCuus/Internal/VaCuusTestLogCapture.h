// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Every GLog line emitted while this object lives, with its verbosity and category, for the
 * two assertions AddExpectedMessage cannot make.
 *
 *  - ABSENCE. An expectation fails when its pattern is NOT met; nothing in the framework fails
 *    when a Warning DOES occur -- an unexpected Warning becomes a Warning event, and is raised
 *    to an Error only under bElevateLogWarningsToErrors (AutomationTest.cpp:128-136), which
 *    defaults to false (:181) and no shipped ini sets. "A routed click logs nothing" needs a
 *    count of zero, which only reading the stream can give.
 *  - LOG LEVEL. The automation output device routes only Error/Warning/Display into the
 *    matching machinery (AutomationTest.cpp:233), so an expectation at ELogVerbosity::Log can
 *    never be satisfied. VaCuusBundlePackTest.cpp carries the same capture for that reason; it
 *    lives in VaCuusEditor, which this module cannot include.
 *
 * Each Serialize call is ONE entry, deliberately: a multi-line message handed to one UE_LOG is
 * one entry with embedded newlines, which is exactly what VaCuus.Log.RmlMultiLine tells apart.
 */
class FVaCuusTestLogCapture final : public FOutputDevice
{
public:
	struct FEntry
	{
		FString Text;
		ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		FName Category;
	};

	FVaCuusTestLogCapture() { GLog->AddOutputDevice(this); }
	virtual ~FVaCuusTestLogCapture() override { GLog->RemoveOutputDevice(this); }

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		FScopeLock Lock(&Mutex);
		Entries.Add({FString(V), ELogVerbosity::Type(Verbosity & ELogVerbosity::VerbosityMask), Category});
	}

	/**
	 * Threaded logging delivers to registered devices from the log thread, so a line emitted
	 * before this call is only guaranteed to be here after the flush.
	 */
	TArray<FEntry> Snapshot()
	{
		GLog->FlushThreadedLogs();
		FScopeLock Lock(&Mutex);
		return Entries;
	}

	/** Entries containing Fragment (case-sensitive), at any verbosity. */
	int32 Count(const TCHAR* Fragment)
	{
		int32 Num = 0;
		for (const FEntry& Entry : Snapshot())
		{
			Num += Entry.Text.Contains(Fragment, ESearchCase::CaseSensitive) ? 1 : 0;
		}
		return Num;
	}

private:
	FCriticalSection Mutex;
	TArray<FEntry> Entries;
};

#endif	  // WITH_DEV_AUTOMATION_TESTS
