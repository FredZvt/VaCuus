// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusBundle.h"
#include "VaCuusContentPaths.h"
#include "VaCuusFileInterface.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <cstdio>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
// Read plugin metadata independently: deriving fixture availability from GetDocumentRoots
// would turn a discovery regression into a successful skip.
TArray<FString> GetExpectedPluginRoots()
{
	TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();
	Plugins.Sort([](const TSharedRef<IPlugin>& A, const TSharedRef<IPlugin>& B) {
		return FCString::Strcmp(*A->GetName(), *B->GetName()) < 0;
	});
	TArray<FString> Expected;
	for (const TSharedRef<IPlugin>& Plugin : Plugins)
	{
		const FString Root = FPaths::ConvertRelativePathToFull(Plugin->GetContentDir() / TEXT("DevUI"));
		if (Plugin->GetName() != TEXT("VaCuus") && IFileManager::Get().DirectoryExists(*Root))
		{
			Expected.Add(Root);
		}
	}
	return Expected;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFileInterfaceTest, "VaCuus.Core.FileInterface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFileInterfaceTest::RunTest(const FString& Parameters)
{
	const FString TestDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusTest"));
	const FString TestFilePath = TestDir / TEXT("file_interface_probe.txt");

	// 16 ASCII characters -> 16 bytes on disk.
	const FString Payload = TEXT("0123456789ABCDEF");
	if (!TestTrue(TEXT("Temp file saved"), FFileHelper::SaveStringToFile(Payload, *TestFilePath)))
	{
		return false;
	}

	const auto ToRmlPath = [](const FString& Path) { return Rml::String(TCHAR_TO_UTF8(*Path)); };

	// Standalone instance; no Rml boot required for the file interface itself.
	FVaCuusFileInterface FileInterface;

	// Open existing file (absolute path passthrough).
	const Rml::FileHandle File = FileInterface.Open(ToRmlPath(TestFilePath));
	if (TestTrue(TEXT("Open existing file returns nonzero handle"), File != Rml::FileHandle(0)))
	{
		// Length.
		TestEqual(TEXT("Length matches payload size"), FileInterface.Length(File), (size_t)16);

		// Valid seek + tell.
		TestTrue(TEXT("Seek SEEK_SET 10 succeeds"), FileInterface.Seek(File, 10, SEEK_SET));
		TestEqual(TEXT("Tell after valid seek"), FileInterface.Tell(File), (size_t)10);

		// Partial read: request more than remaining -> returns remaining count
		// (Rml::FileInterface::Read contract: "total number of bytes read").
		char Buffer[64] = {0};
		TestEqual(TEXT("Read request past EOF returns remaining byte count"), FileInterface.Read(Buffer, sizeof(Buffer), File), (size_t)6);
		TestTrue(TEXT("Read content matches payload tail"), FMemory::Memcmp(Buffer, "ABCDEF", 6) == 0);
		TestEqual(TEXT("Read at EOF returns 0"), FileInterface.Read(Buffer, sizeof(Buffer), File), (size_t)0);
		TestEqual(TEXT("Tell after reading to EOF"), FileInterface.Tell(File), (size_t)16);

		// SEEK_END with negative offset lands back in range.
		TestTrue(TEXT("Seek SEEK_END -16 succeeds"), FileInterface.Seek(File, -16, SEEK_END));
		TestEqual(TEXT("Tell after SEEK_END -16"), FileInterface.Tell(File), (size_t)0);

		// Out-of-range seeks must fail without touching IFileHandle (which asserts).
		TestFalse(TEXT("Seek SEEK_SET beyond size fails"), FileInterface.Seek(File, 17, SEEK_SET));
		TestFalse(TEXT("Seek SEEK_SET negative fails"), FileInterface.Seek(File, -1, SEEK_SET));
		TestFalse(TEXT("Seek SEEK_CUR below zero fails"), FileInterface.Seek(File, -1, SEEK_CUR));
		TestFalse(TEXT("Seek SEEK_END past end fails"), FileInterface.Seek(File, 1, SEEK_END));
		TestEqual(TEXT("Tell unchanged after failed seeks"), FileInterface.Tell(File), (size_t)0);

		//~ EXACT EOF -- the one position IFileHandle cannot hold, and the reason this
		//~ interface tracks the read position itself. FFileHandleUnix::Seek clamps a
		//~ read-mode seek to the LAST BYTE
		//~ (Runtime/Core/Private/Unix/UnixPlatformFile.cpp:177) and Tell() returns that
		//~ clamped member (:152-157), so answering Tell() from the handle reports 15 here
		//~ and the read below hands the last byte back a SECOND time instead of nothing.
		//~ Rml::FileInterface::Tell is specified as bytes-from-the-origin with no clamp
		//~ (ThirdParty/RmlUi/Include/RmlUi/Core/FileInterface.h:43-46), and RmlUi's own
		//~ default Length() is Seek-to-end plus Tell (:48-52) -- so one short here is a
		//~ silently truncated document.
		TestTrue(TEXT("Seek to exact EOF succeeds"), FileInterface.Seek(File, 0, SEEK_END));
		TestEqual(TEXT("EXACT EOF: Tell is Size, not Size - 1"), FileInterface.Tell(File), (size_t)16);
		TestEqual(TEXT("EXACT EOF: a read returns 0, not the last byte again"),
			FileInterface.Read(Buffer, sizeof(Buffer), File), (size_t)0);
		TestEqual(TEXT("EXACT EOF: the refused read did not move the position"), FileInterface.Tell(File), (size_t)16);

		// A seek past the end from exact EOF is still refused, and still leaves the
		// position where Tell() says it is -- the property that lets Read() re-sync the
		// underlying handle from it.
		TestFalse(TEXT("EXACT EOF: SEEK_CUR +1 is still out of range"), FileInterface.Seek(File, 1, SEEK_CUR));
		TestEqual(TEXT("EXACT EOF: Tell survives that too"), FileInterface.Tell(File), (size_t)16);

		// SEEK_CUR back from exact EOF: only correct if Seek and Tell agree about where
		// EOF is, and it proves the handle re-syncs rather than reading from wherever the
		// clamp left it.
		TestTrue(TEXT("Seek SEEK_CUR -6 from exact EOF succeeds"), FileInterface.Seek(File, -6, SEEK_CUR));
		TestEqual(TEXT("...landing 6 bytes from the end"), FileInterface.Tell(File), (size_t)10);
		FMemory::Memzero(Buffer, sizeof(Buffer));
		TestEqual(TEXT("...and the tail still reads back"), FileInterface.Read(Buffer, sizeof(Buffer), File), (size_t)6);
		TestTrue(TEXT("...with the right bytes"), FMemory::Memcmp(Buffer, "ABCDEF", 6) == 0);

		// And the whole file re-reads from the start, so nothing above left the handle
		// desynchronised from the logical position.
		TestTrue(TEXT("Rewind to 0"), FileInterface.Seek(File, 0, SEEK_SET));
		FMemory::Memzero(Buffer, sizeof(Buffer));
		TestEqual(TEXT("The whole file reads back after all that seeking"),
			FileInterface.Read(Buffer, sizeof(Buffer), File), (size_t)16);
		TestTrue(TEXT("...byte for byte"), FMemory::Memcmp(Buffer, "0123456789ABCDEF", 16) == 0);

		FileInterface.Close(File);
	}

	//~ THE ZERO-LENGTH FILE, which is the same clamp at its worst: FileSize - 1 is -1, so
	//~ the handle's Tell() returns a NEGATIVE position and the size_t cast turns it into
	//~ ~0ULL. Not hypothetical for a live-reload tree -- a just-created .rcss, or one
	//~ caught mid-save, is exactly this file.
	{
		const FString EmptyPath = TestDir / TEXT("file_interface_empty.txt");
		if (TestTrue(TEXT("Empty file saved"), FFileHelper::SaveStringToFile(FString(), *EmptyPath)))
		{
			const Rml::FileHandle Empty = FileInterface.Open(ToRmlPath(EmptyPath));
			if (TestTrue(TEXT("Empty file opens"), Empty != Rml::FileHandle(0)))
			{
				char Buffer[8] = {0};
				TestEqual(TEXT("Empty file length is 0"), FileInterface.Length(Empty), (size_t)0);
				TestEqual(TEXT("Empty file starts at 0"), FileInterface.Tell(Empty), (size_t)0);
				TestTrue(TEXT("Seek to EOF of an empty file succeeds"), FileInterface.Seek(Empty, 0, SEEK_END));
				TestEqual(TEXT("EOF of an empty file is 0, not a huge unsigned"),
					FileInterface.Tell(Empty), (size_t)0);
				TestEqual(TEXT("Reading an empty file returns 0"),
					FileInterface.Read(Buffer, sizeof(Buffer), Empty), (size_t)0);
				TestFalse(TEXT("Seeking past the end of an empty file fails"), FileInterface.Seek(Empty, 1, SEEK_SET));
				FileInterface.Close(Empty);
			}
			IFileManager::Get().Delete(*EmptyPath);
		}
	}

	// Directories and missing files must not produce handles.
	TestEqual(TEXT("Open directory returns 0"), FileInterface.Open(ToRmlPath(TestDir)), Rml::FileHandle(0));
	TestEqual(TEXT("Open missing file returns 0"), FileInterface.Open(ToRmlPath(TestDir / TEXT("does_not_exist.txt"))), Rml::FileHandle(0));

	// Clean up.
	IFileManager::Get().Delete(*TestFilePath);
	IFileManager::Get().DeleteDirectory(*TestDir);

	return true;
}

/**
 * The ordered DevUI roots and the precedence between them (controller decision D19,
 * bead VaCuus-akj.6.3).
 *
 * WHAT THIS IS REALLY GUARDING: the plugin's Content/DevUI is now canonical, and the
 * duplicated copy under <Project>/Content/DevUI has been deleted. If the order ever
 * flipped, a project copy someone re-creates would silently shadow the plugin document
 * the editor file watcher is watching -- live reload would stop working with no error
 * anywhere. So the order is asserted, not just the resolution.
 *
 * Discovery is checked against enabled plugin metadata, including completeness and
 * name order. Tools/plugin_roots_check.py installs companion plugins before startup
 * so this path cannot go unexercised in a host containing only VaCuus.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusContentRootsTest, "VaCuus.Core.ContentRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusContentRootsTest::RunTest(const FString& Parameters)
{
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (!TestTrue(TEXT("At least the VaCuus and project DevUI roots"), Roots.Num() >= 2))
	{
		return false;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VaCuus"));
	if (!TestTrue(TEXT("VaCuus plugin descriptor found"), Plugin.IsValid()))
	{
		return false;
	}

	const FString ExpectedPluginRoot =
		FPaths::ConvertRelativePathToFull(Plugin->GetContentDir() / TEXT("DevUI"));
	const FString ExpectedProjectRoot =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("DevUI"));

	TestEqual(TEXT("The FIRST root is VaCuus's own Content/DevUI"), Roots[0], ExpectedPluginRoot);
	TestEqual(TEXT("The LAST root is the PROJECT's Content/DevUI"), Roots.Last(), ExpectedProjectRoot);

	// Check completeness as well as membership: a resolver that drops every extra
	// plugin must fail even though its surviving roots are individually valid.
	TArray<FString> ExpectedRoots = {ExpectedPluginRoot};
	TArray<FString> RemainingRoots = GetExpectedPluginRoots();
	RemainingRoots.Add(ExpectedProjectRoot);
	for (const FString& Root : RemainingRoots)
	{
		if (!ExpectedRoots.ContainsByPredicate([&Root](const FString& Existing) {
				return Existing.Equals(Root, ESearchCase::CaseSensitive);
			}))
		{
			ExpectedRoots.Add(Root);
		}
	}
	if (TestEqual(TEXT("All eligible plugin roots are discovered, without duplicates"), Roots.Num(), ExpectedRoots.Num()))
	{
		for (int32 Index = 0; Index < Roots.Num(); ++Index)
		{
			TestTrue(*FString::Printf(TEXT("Root %d preserves plugin-name order and path case: expected '%s', got '%s'"),
				Index, *ExpectedRoots[Index], *Roots[Index]),
				Roots[Index].Equals(ExpectedRoots[Index], ESearchCase::CaseSensitive));
		}
	}

	// The shipped HUD document must resolve, and must resolve to the PLUGIN copy: that is
	// the concrete claim "the plugin's Content/DevUI is canonical" makes.
	FString HudRoot;
	const FString HudPath = VaCuusContentPaths::ResolveExistingDocument(TEXT("m1_hud.rml"), &HudRoot);
	TestFalse(TEXT("m1_hud.rml resolves"), HudPath.IsEmpty());
	TestEqual(TEXT("m1_hud.rml comes from the plugin root"), HudRoot, ExpectedPluginRoot);

	// Precedence, proved by shadowing rather than asserted: the same relative name exists
	// under both roots with different contents, and the plugin's must win.
	const FString ProbeName = TEXT("vacuus_root_order_probe.rml.tmptest");
	const FString PluginProbe = Roots[0] / ProbeName;
	const FString ProjectProbe = Roots.Last() / ProbeName;

	const bool bWrotePlugin = FFileHelper::SaveStringToFile(TEXT("PLUGIN"), *PluginProbe);
	const bool bWroteProject = FFileHelper::SaveStringToFile(TEXT("PROJECTROOT"), *ProjectProbe);
	if (TestTrue(TEXT("Probe written under both roots"), bWrotePlugin && bWroteProject))
	{
		FVaCuusFileInterface FileInterface;
		const auto ToRmlPath = [](const FString& Path) { return Rml::String(TCHAR_TO_UTF8(*Path)); };

		// Length is the discriminator: "PLUGIN" is 6 bytes, "PROJECTROOT" is 11.
		const Rml::FileHandle Shadowed = FileInterface.Open(ToRmlPath(ProbeName));
		if (TestTrue(TEXT("Shadowed probe opens"), Shadowed != Rml::FileHandle(0)))
		{
			TestEqual(TEXT("The PLUGIN copy wins when both roots have the file"),
				FileInterface.Length(Shadowed), (size_t)6);
			FileInterface.Close(Shadowed);
		}

		// With the plugin copy gone, the project root is a real fallback rather than dead
		// code -- that is the "extension point" half of D19.
		IFileManager::Get().Delete(*PluginProbe);

		const Rml::FileHandle Fallback = FileInterface.Open(ToRmlPath(ProbeName));
		if (TestTrue(TEXT("Project-root probe opens once the plugin copy is gone"), Fallback != Rml::FileHandle(0)))
		{
			TestEqual(TEXT("The PROJECT copy answers as the second root"),
				FileInterface.Length(Fallback), (size_t)11);
			FileInterface.Close(Fallback);
		}

		// A relative name under no root at all must not produce a handle.
		TestEqual(TEXT("Unknown relative name returns 0"),
			FileInterface.Open(ToRmlPath(TEXT("vacuus_no_such_document.rml"))), Rml::FileHandle(0));
	}

	IFileManager::Get().Delete(*PluginProbe);
	IFileManager::Get().Delete(*ProjectProbe);

	// The project DevUI directory may not have existed before this test created the probe
	// in it (it is deliberately empty in this repo now); leave it as we found it.
	if (IFileManager::Get().DirectoryExists(*Roots.Last()))
	{
		TArray<FString> Remaining;
		IFileManager::Get().FindFilesRecursive(Remaining, *Roots.Last(), TEXT("*"), true, true);
		if (Remaining.Num() == 0)
		{
			IFileManager::Get().DeleteDirectory(*Roots.Last());
		}
	}

	return true;
}

/**
 * ProbeImage: the two ways art goes missing, told apart (bead VaCuus-akj.28).
 *
 * WHY AN EXISTENCE CHECK IS NOT ENOUGH, which is the whole reason this function exists
 * rather than a FPaths::FileExists at each call site. Both this repo and VaCuusDemo
 * carry `*.png filter=lfs` in .gitattributes, so a clone made on a machine WITHOUT
 * git-lfs installed writes ~130-byte POINTER FILES in place of every image. The file is
 * there, stat succeeds, the open succeeds, and the failure surfaces two layers down in
 * the PNG decoder as the same blank rectangles a wholly absent file produces. The
 * incident this bead was filed from cost an hour on the wrong hypothesis (an async
 * decode race) because the two look identical on screen.
 *
 * So the assertion that matters here is not "does the probe notice" but "does the probe
 * NAME THE REPAIR": a pointer file must produce a diagnosis carrying `git lfs pull`,
 * because that string is the entire difference between a one-line fix and an hour.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusImageProbeTest, "VaCuus.Core.ImageProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusImageProbeTest::RunTest(const FString& Parameters)
{
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (!TestTrue(TEXT("At least one DevUI root"), Roots.Num() > 0))
	{
		return false;
	}

	// Relative names, resolved through the roots exactly as the demo bootstraps do -- an
	// absolute-path test would skip the resolution half and prove less than it looks.
	const FString ProbeName = TEXT("vacuus_image_probe.png.tmptest");
	const FString ProbePath = Roots[0] / ProbeName;
	IFileManager::Get().Delete(*ProbePath);

	// --- Absent: nothing under any root. ---
	{
		FString Diagnosis;
		TestEqual(TEXT("A name under no root probes as Missing"),
			VaCuusContentPaths::ProbeImage(ProbeName, &Diagnosis), EVaCuusImageProbe::Missing);
		TestTrue(TEXT("The Missing diagnosis names the file"), Diagnosis.Contains(ProbeName));
	}

	// --- A Git-LFS pointer: present, readable, and NOT an image. ---
	{
		// The real v1 pointer layout (three lines, LF-terminated) rather than a
		// hand-waved prefix -- a smudge-less checkout writes exactly this.
		const FString Pointer =
			TEXT("version https://git-lfs.github.com/spec/v1\n")
			TEXT("oid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\n")
			TEXT("size 12345\n");
		if (TestTrue(TEXT("Pointer fixture written"), FFileHelper::SaveStringToFile(Pointer, *ProbePath)))
		{
			FString Diagnosis;
			TestEqual(TEXT("A pointer file probes as GitLfsPointer, not as a valid image"),
				VaCuusContentPaths::ProbeImage(ProbeName, &Diagnosis), EVaCuusImageProbe::GitLfsPointer);

			// The point of the whole bead: the message must carry the repair.
			TestTrue(TEXT("The pointer diagnosis names the repair 'git lfs pull'"),
				Diagnosis.Contains(TEXT("git lfs pull")));
		}
		IFileManager::Get().Delete(*ProbePath);
	}

	// --- Present, readable, neither a pointer nor an image: truncated or corrupt. ---
	{
		if (TestTrue(TEXT("Garbage fixture written"), FFileHelper::SaveStringToFile(TEXT("not an image at all"), *ProbePath)))
		{
			FString Diagnosis;
			TestEqual(TEXT("A non-image file probes as NotAnImage"),
				VaCuusContentPaths::ProbeImage(ProbeName, &Diagnosis), EVaCuusImageProbe::NotAnImage);
		}
		IFileManager::Get().Delete(*ProbePath);
	}

	// --- A real PNG signature. ---
	{
		const uint8 PngBytes[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D};
		TArray<uint8> Bytes(PngBytes, UE_ARRAY_COUNT(PngBytes));
		if (TestTrue(TEXT("PNG fixture written"), FFileHelper::SaveArrayToFile(Bytes, *ProbePath)))
		{
			FString Diagnosis;
			TestEqual(TEXT("A PNG-signed file probes as Ok"),
				VaCuusContentPaths::ProbeImage(ProbeName, &Diagnosis), EVaCuusImageProbe::Ok);
			TestTrue(TEXT("An Ok probe reports no diagnosis"), Diagnosis.IsEmpty());
		}
		IFileManager::Get().Delete(*ProbePath);
	}

	// --- The plugin's OWN shipped art, over the real resolution path. ---
	// m1_hud.rml references exactly this one image; if this repo is ever cloned without
	// git-lfs, or the file is dropped, this row is what says so.
	{
		FString Diagnosis;
		TestEqual(TEXT("The shipped img/avatar.png probes as Ok"),
			VaCuusContentPaths::ProbeImage(TEXT("img/avatar.png"), &Diagnosis), EVaCuusImageProbe::Ok);
	}

	return true;
}

/**
 * The ORDER RULE itself, on inputs this test chooses.
 *
 * WHY IT IS SEPARATE FROM VaCuus.Core.ContentRoots: tier 2 is auto-discovered, so what
 * GetDocumentRoots() returns depends on which plugins are installed on the machine running
 * the suite. A test that can only assert the two ends cannot assert the thing the feature
 * IS -- that a plugin root sits after VaCuus and before the project, that the order inside
 * tier 2 is the caller's, and that nothing is listed twice. ComposeDocumentRoots exists to
 * make those assertable without installing anything.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusContentRootCompositionTest, "VaCuus.Core.ContentRootComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusContentRootCompositionTest::RunTest(const FString& Parameters)
{
	const FString VaCuusContent = TEXT("/x/Plugins/VaCuus/Content");
	const FString PluginAContent = TEXT("/x/Plugins/Aardvark/Content");
	const FString PluginZContent = TEXT("/x/Plugins/Zebra/Content");
	const FString ProjectContent = TEXT("/x/Content");

	const auto Root = [](const FString& ContentDir) {
		return FPaths::ConvertRelativePathToFull(ContentDir / TEXT("DevUI"));
	};

	// The three tiers, in order, with tier 2 consumed exactly as handed over.
	{
		const TArray<FString> Composed = VaCuusContentPaths::ComposeDocumentRoots(
			VaCuusContent, {PluginAContent, PluginZContent}, ProjectContent);

		if (TestEqual(TEXT("Four roots"), Composed.Num(), 4))
		{
			TestEqual(TEXT("VaCuus is first"), Composed[0], Root(VaCuusContent));
			TestEqual(TEXT("Then the plugins, in the given order"), Composed[1], Root(PluginAContent));
			TestEqual(TEXT("Then the plugins, in the given order"), Composed[2], Root(PluginZContent));
			TestEqual(TEXT("The project is last"), Composed.Last(), Root(ProjectContent));
		}
	}

	// VaCuus listed again among the plugins must not produce a second root. Not a contrived
	// case: the discovery scan skips it by name, and this is what says so if that skip is
	// ever lost.
	{
		const TArray<FString> Composed = VaCuusContentPaths::ComposeDocumentRoots(
			VaCuusContent, {VaCuusContent, PluginAContent}, ProjectContent);

		if (TestEqual(TEXT("The duplicate collapses"), Composed.Num(), 3))
		{
			TestEqual(TEXT("VaCuus keeps its FIRST position, not a later one"), Composed[0], Root(VaCuusContent));
			TestEqual(TEXT("The real plugin follows"), Composed[1], Root(PluginAContent));
			TestEqual(TEXT("The project is still last"), Composed.Last(), Root(ProjectContent));
		}
	}

	// A plugin mounted under the project's own Content directory: one directory, one root,
	// and it must not be demoted to the project's slot.
	{
		const TArray<FString> Composed =
			VaCuusContentPaths::ComposeDocumentRoots(VaCuusContent, {ProjectContent}, ProjectContent);

		if (TestEqual(TEXT("The shared directory is listed once"), Composed.Num(), 2))
		{
			TestEqual(TEXT("VaCuus first"), Composed[0], Root(VaCuusContent));
			TestEqual(TEXT("The shared root second"), Composed[1], Root(ProjectContent));
		}
	}

	// A different spelling can be a different physical root; preserve it even on
	// machines where this particular fixture would alias the original directory.
	{
		const FString CaseVariant = TEXT("/x/plugins/vacuus/content");
		const TArray<FString> Composed = VaCuusContentPaths::ComposeDocumentRoots(
			VaCuusContent, {CaseVariant}, ProjectContent);

		if (TestEqual(TEXT("Case-only root variants are preserved"), Composed.Num(), 3))
		{
			TestTrue(TEXT("The additional root keeps its spelling"),
				Composed[1].Equals(Root(CaseVariant), ESearchCase::CaseSensitive));
		}
	}

#if WITH_EDITOR
	// Exercise the consumer that loses data when two physical roots are folded together.
	const FString CaseDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusTest/CaseRoots"));
	const FString UpperContent = CaseDir / TEXT("Plugin/Content");
	const FString LowerContent = CaseDir / TEXT("plugin/Content");
	if (TestTrue(TEXT("Case-root pack fixtures written"),
			FFileHelper::SaveStringToFile(TEXT("UPPER"), *(Root(UpperContent) / TEXT("upper_unique.rml"))) &&
			FFileHelper::SaveStringToFile(TEXT("lower"), *(Root(LowerContent) / TEXT("lower_unique.rml")))))
	{
		const TArray<FString> Composed = VaCuusContentPaths::ComposeDocumentRoots(
			FString(), {UpperContent, LowerContent}, FString());
		const TArray<VaCuusBundlePack::FSourceFile> Files = VaCuusBundlePack::EnumerateTree(Composed);
		TestTrue(TEXT("Upper root's unique file reaches the bundle"), Files.ContainsByPredicate([](const auto& File) {
			return File.NormalizedPath == TEXT("upper_unique.rml");
		}));
		TestTrue(TEXT("Lower root's unique file reaches the bundle"), Files.ContainsByPredicate([](const auto& File) {
			return File.NormalizedPath == TEXT("lower_unique.rml");
		}));
	}
	IFileManager::Get().DeleteDirectory(*CaseDir, false, true);
#endif

	// An empty tier does not shift the others: a missing VaCuus descriptor is a logged Error,
	// not a reason for the first discovered plugin to inherit tier 1's precedence.
	{
		const TArray<FString> Composed =
			VaCuusContentPaths::ComposeDocumentRoots(FString(), {PluginAContent}, ProjectContent);

		if (TestEqual(TEXT("Two roots when VaCuus has no content dir"), Composed.Num(), 2))
		{
			TestEqual(TEXT("The plugin is first"), Composed[0], Root(PluginAContent));
			TestEqual(TEXT("The project is last"), Composed.Last(), Root(ProjectContent));
		}
	}

	TestEqual(TEXT("No roots at all when nothing is supplied"),
		VaCuusContentPaths::ComposeDocumentRoots(FString(), {}, FString()).Num(), 0);

	return true;
}

/** Bundle collision diagnostics must not claim that a loose file is unreachable. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusShadowedDocumentsTest, "VaCuus.Core.ShadowedDocuments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusShadowedDocumentsTest::RunTest(const FString& Parameters)
{
	// Two roots of this test's own making, under Saved/: scanning the REAL roots would make
	// the result depend on what the repo and the machine happen to carry.
	const FString ScanDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusTest") / TEXT("Shadow"));
	const FString RootA = ScanDir / TEXT("A");
	const FString RootB = ScanDir / TEXT("B");
	const TArray<FString> Roots = {RootA, RootB};

	const FString Shared = TEXT("Screens/menu.rml");
	const FString OnlyInB = TEXT("Screens/only_b.rml");

	const bool bWrote = FFileHelper::SaveStringToFile(TEXT("A"), *(RootA / Shared)) &&
						FFileHelper::SaveStringToFile(TEXT("B"), *(RootB / Shared)) &&
						FFileHelper::SaveStringToFile(TEXT("B"), *(RootB / OnlyInB));
	if (!TestTrue(TEXT("Probe tree written"), bWrote))
	{
		IFileManager::Get().DeleteDirectory(*ScanDir, false, true);
		return false;
	}

	AddExpectedMessagePlain(TEXT("Bundle path 'screens/menu.rml' has a collision:"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	{
		TArray<FString> Shadowed;
		const int32 NumShadowed = VaCuusContentPaths::ScanShadowedDocuments(Roots, &Shadowed);

		TestEqual(TEXT("Exactly one document is shadowed"), NumShadowed, 1);
		if (TestEqual(TEXT("...and it is reported"), Shadowed.Num(), 1))
		{
			// NormalizePath lowercases, so that is the spelling the report carries.
			TestEqual(TEXT("The shadowed path is the shared one, normalized"), Shadowed[0],
				VaCuusBundleFormat::NormalizePath(Shared));
		}
	}

	// A Tests/ fixture is per-root by design and never addressed by a document, so a
	// collision between two of them is not a fault. This row is what keeps that exclusion
	// from being quietly dropped.
	{
		const FString Fixture = TEXT("Tests/fixture.rml");
		if (TestTrue(TEXT("Fixture pair written"),
				FFileHelper::SaveStringToFile(TEXT("A"), *(RootA / Fixture)) &&
					FFileHelper::SaveStringToFile(TEXT("B"), *(RootB / Fixture))))
		{
			TestEqual(TEXT("A shadowed Tests/ fixture is not reported"),
				VaCuusContentPaths::ScanShadowedDocuments(Roots), 1);
		}
	}

	// Deleting one copy removes the collision without removing the remaining document.
	IFileManager::Get().Delete(*(RootA / Shared));
	{
		TArray<FString> Shadowed;
		TestEqual(TEXT("Nothing is shadowed once one copy is deleted"),
			VaCuusContentPaths::ScanShadowedDocuments(Roots, &Shadowed), 0);
		TestEqual(TEXT("...and nothing is reported"), Shadowed.Num(), 0);
	}

	// These distinct paths work on case-insensitive volumes too. Normalization folds
	// their VFS names together, while the file interface can still open either loose file.
	const FString Upper = RootA / TEXT("CaseOnly/Panel.rml");
	const FString Lower = RootB / TEXT("CaseOnly/panel.rml");
	if (TestTrue(TEXT("Case-only collision fixtures written"),
			FFileHelper::SaveStringToFile(TEXT("A"), *Upper) &&
			FFileHelper::SaveStringToFile(TEXT("lower"), *Lower)))
	{
		AddExpectedMessagePlain(
			FString::Printf(TEXT("Bundle path 'caseonly/panel.rml' has a collision: '%s' wins when packed; ")
				TEXT("'%s' is excluded from the bundle. Loose-file lookup can differ, especially for case-only names."),
				*Upper, *Lower), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		TArray<FString> Shadowed;
		TestEqual(TEXT("Case-only VFS names collide in the bundle"),
			VaCuusContentPaths::ScanShadowedDocuments(Roots, &Shadowed), 1);
		if (TestEqual(TEXT("The normalized collision is reported"), Shadowed.Num(), 1))
		{
			TestTrue(TEXT("Collision path is lowercase"),
				Shadowed[0].Equals(TEXT("caseonly/panel.rml"), ESearchCase::CaseSensitive));
		}
		FVaCuusFileInterface FileInterface;
		const Rml::FileHandle Handle = FileInterface.Open(Rml::String(TCHAR_TO_UTF8(*Lower)));
		if (TestTrue(TEXT("The bundle-excluded copy remains readable loose"), Handle != Rml::FileHandle(0)))
		{
			TestEqual(TEXT("Loose read returns the excluded copy's bytes"), FileInterface.Length(Handle), size_t(5));
			FileInterface.Close(Handle);
		}
	}

	// A root that does not exist is not an error: tiers 1 and 3 are listed unconditionally,
	// so the scan is handed non-existent directories on an ordinary boot.
	TestEqual(TEXT("A missing root contributes nothing"),
		VaCuusContentPaths::ScanShadowedDocuments({ScanDir / TEXT("NoSuchRoot")}), 0);

	IFileManager::Get().DeleteDirectory(*ScanDir, false, true);
	return true;
}

/**
 * A PLUGIN root, end to end: discovered, served, precedent over the project, and packed.
 *
 * Fixture availability comes from plugin metadata, never the root list under test.
 * Tools/plugin_roots_check.py supplies companion plugins before the process starts.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusPluginDocumentRootTest, "VaCuus.Core.PluginDocumentRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusPluginDocumentRootTest::RunTest(const FString& Parameters)
{
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	const TArray<FString> ExpectedPluginRoots = GetExpectedPluginRoots();
	if (ExpectedPluginRoots.IsEmpty())
	{
		AddInfo(TEXT("Skipped: enabled plugin metadata contains no additional DevUI directory. ")
			TEXT("Run Tools/plugin_roots_check.py to supply companion plugins"));
		return true;
	}

	const FString PluginRoot = ExpectedPluginRoots[0];
	if (!TestTrue(TEXT("The independently identified plugin root is discovered"),
			Roots.ContainsByPredicate([&PluginRoot](const FString& Root) {
				return Root.Equals(PluginRoot, ESearchCase::CaseSensitive);
			})))
	{
		return false;
	}
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("DevUI"));

	// ".tmptest" keeps it out of every staging glob and out of GetPackedExtensions, so a
	// crashed run cannot leave a file the cooker would pick up.
	const FString ProbeName = TEXT("vacuus_plugin_root_probe.rml.tmptest");
	const FString PluginProbe = PluginRoot / ProbeName;
	const FString ProjectProbe = ProjectRoot / ProbeName;

	// 12 bytes vs 7: length is the discriminator, as in VaCuus.Core.ContentRoots.
	const bool bWrote = FFileHelper::SaveStringToFile(TEXT("PLUGINROOTED"), *PluginProbe) &&
						FFileHelper::SaveStringToFile(TEXT("PROJECT"), *ProjectProbe);
	if (TestTrue(TEXT("Probe written under the plugin root and the project root"), bWrote))
	{
		FVaCuusFileInterface FileInterface;
		const auto ToRmlPath = [](const FString& Path) { return Rml::String(TCHAR_TO_UTF8(*Path)); };

		const Rml::FileHandle Handle = FileInterface.Open(ToRmlPath(ProbeName));
		if (TestTrue(TEXT("A document in a plugin's Content/DevUI opens through the VFS"),
				Handle != Rml::FileHandle(0)))
		{
			TestEqual(TEXT("The PLUGIN copy wins over the project's"), FileInterface.Length(Handle), (size_t)12);
			FileInterface.Close(Handle);
		}

		FString SatisfyingRoot;
		VaCuusContentPaths::ResolveExistingDocument(ProbeName, &SatisfyingRoot);
		TestEqual(TEXT("...and the resolver names that plugin root"), SatisfyingRoot, PluginRoot);
	}

	IFileManager::Get().Delete(*PluginProbe);
	IFileManager::Get().Delete(*ProjectProbe);

#if WITH_EDITOR
	// A real extension is required to exercise the Shipping pack's discovery path.
	// The external check supplies disposable plugins, so even an interrupted run can
	// clean up the fixture without leaving a packable document in a consumer's tree.
	{
		const FString PackProbeName = TEXT("vacuus_plugin_root_pack_probe.rml");
		const FString PackProbe = PluginRoot / PackProbeName;
		if (TestTrue(TEXT("Pack probe written under the plugin root"),
				FFileHelper::SaveStringToFile(TEXT("<rml/>"), *PackProbe)))
		{
			const FString Expected = VaCuusBundleFormat::NormalizePath(PackProbeName);
			bool bFound = false;
			for (const VaCuusBundlePack::FSourceFile& File : VaCuusBundlePack::EnumerateTree(Roots))
			{
				if (File.NormalizedPath == Expected)
				{
					bFound = true;
					TestEqual(TEXT("...claimed from the plugin root's copy"),
						File.DiskPath, FPaths::ConvertRelativePathToFull(PackProbe));
					break;
				}
			}
			TestTrue(TEXT("The bundle pack claims a document from the plugin root"), bFound);
		}
		IFileManager::Get().Delete(*PackProbe);
	}
#endif // WITH_EDITOR

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
