// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusContentPaths.h"

#include "VaCuusBundle.h"
#include "VaCuusBundleMount.h"
#include "VaCuusDefines.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace VaCuusContentPaths
{
namespace Private
{
/** Subdirectory every root shares; the documents themselves are addressed relative to it. */
static const TCHAR* GDevUISubDir = TEXT("DevUI");

/** Tier 1, and the one plugin excluded from the tier-2 scan so it cannot be listed twice. */
static const TCHAR* GVaCuusPluginName = TEXT("VaCuus");

/** One content directory -> one absolute DevUI root. The only place the two are joined. */
static FString ToDevUIRoot(const FString& ContentDir)
{
	return FPaths::ConvertRelativePathToFull(ContentDir / GDevUISubDir);
}

/** A tier-2 candidate: the name is what orders it, the content dir is what becomes the root. */
struct FDiscoveredPluginRoot
{
	FString PluginName;
	FString ContentDir;
};

/**
 * Every enabled plugin EXCEPT VaCuus that actually has a Content/DevUI directory, sorted by
 * plugin name.
 *
 * SORTED, because GetEnabledPlugins() hands back discovery order -- which depends on where a
 * plugin lives (project, engine, additional directory) and on the filesystem's enumeration
 * order inside each. Precedence between two plugins that ship the same document path would
 * otherwise be decided by something neither author can see, and it would be free to change
 * between machines. FCString::Strcmp, not FString::operator<, for the reason
 * VaCuusBundlePack::EnumerateTree records at length: the latter compares case-insensitively
 * and would answer "equal" for the one pair that can still collide, handing the tiebreak
 * back to enumeration order.
 */
static TArray<FDiscoveredPluginRoot> DiscoverPluginRoots(int32& OutNumScanned)
{
	TArray<FDiscoveredPluginRoot> Discovered;
	IFileManager& FileManager = IFileManager::Get();

	const TArray<TSharedRef<IPlugin>> EnabledPlugins = IPluginManager::Get().GetEnabledPlugins();
	OutNumScanned = EnabledPlugins.Num();

	for (const TSharedRef<IPlugin>& Plugin : EnabledPlugins)
	{
		if (Plugin->GetName() == GVaCuusPluginName)
		{
			continue;
		}

		// THE EXISTENCE GATE (VaCuusContentPaths.h): ~100 enabled plugins is normal and
		// ResolveExistingDocument stats every root before reporting a miss, so an ungated
		// list would be paid for on every unresolved path.
		const FString Candidate = ToDevUIRoot(Plugin->GetContentDir());
		if (!FileManager.DirectoryExists(*Candidate))
		{
			// Verbose, not Log: this is the answer for almost every plugin in the build, and
			// at Log it would bury the roots line it exists to explain.
			UE_LOG(LogVaCuus, Verbose, TEXT("Plugin '%s' contributes no document root ('%s' does not exist)"),
				*Plugin->GetName(), *Candidate);
			continue;
		}

		Discovered.Add(FDiscoveredPluginRoot{Plugin->GetName(), Plugin->GetContentDir()});
	}

	Discovered.Sort([](const FDiscoveredPluginRoot& A, const FDiscoveredPluginRoot& B) {
		return FCString::Strcmp(*A.PluginName, *B.PluginName) < 0;
	});

	return Discovered;
}

static TArray<FString> BuildDocumentRoots()
{
	// 1. VaCuus's own content -- canonical (D19).
	FString VaCuusContentDir;
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(GVaCuusPluginName))
	{
		VaCuusContentDir = Plugin->GetContentDir();
	}
	else
	{
		// Not fatal: the other roots are still valid places for documents, and a missing
		// descriptor means something much larger is wrong (VaCuusRender's StartupModule
		// check()s on the same lookup for its shader directory).
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus plugin descriptor not found, so VaCuus's own Content/DevUI cannot be a document root; ")
			TEXT("only discovered plugin roots and <Project>/Content/DevUI will be searched"));
	}

	// 2. Every other enabled plugin that ships documents.
	int32 NumScanned = 0;
	const TArray<FDiscoveredPluginRoot> Discovered = DiscoverPluginRoots(NumScanned);

	TArray<FString> OtherPluginContentDirs;
	OtherPluginContentDirs.Reserve(Discovered.Num());
	TArray<FString> DiscoveredNames;
	DiscoveredNames.Reserve(Discovered.Num());
	for (const FDiscoveredPluginRoot& Root : Discovered)
	{
		OtherPluginContentDirs.Add(Root.ContentDir);
		DiscoveredNames.Add(Root.PluginName);
	}

	// 3. The project's, for documents a project adds itself.
	TArray<FString> Roots = ComposeDocumentRoots(VaCuusContentDir, OtherPluginContentDirs, FPaths::ProjectContentDir());

	// The wording of this line is load-bearing -- live reload's "if reload seems dead, check
	// that the file you edited is under a root named above" points at it, and the docs quote
	// it -- so it stays exactly as it was when there were two roots.
	UE_LOG(LogVaCuus, Log, TEXT("VaCuus document roots (in order): %s"), *FString::Join(Roots, TEXT(" | ")));

	// And WHICH plugins put a root in that list, because the list itself shows directories and
	// a reader asking "why is that one there" has no other way to find out. Unconditional even
	// at zero: "none of your plugins ship documents" is the answer to the commonest question
	// about this feature, and a staged-but-pruned Shipping index can produce it unexpectedly
	// (VaCuusContentPaths.h).
	UE_LOG(LogVaCuus, Log, TEXT("VaCuus found %d plugin document root(s) among %d enabled plugin(s)%s%s"),
		DiscoveredNames.Num(), NumScanned, DiscoveredNames.Num() > 0 ? TEXT(": ") : TEXT(""),
		*FString::Join(DiscoveredNames, TEXT(", ")));

	return Roots;
}
}	 // namespace Private

TArray<FString> ComposeDocumentRoots(const FString& VaCuusContentDir,
	const TArray<FString>& OtherPluginContentDirs, const FString& ProjectContentDir)
{
	TArray<FString> Roots;
	Roots.Reserve(OtherPluginContentDirs.Num() + 2);

	// FString equality ignores case (UnrealString.h.inl:906-915), but physical roots
	// can be distinct on a case-sensitive volume. Keeping two aliases costs a lookup;
	// folding two different directories loses files from the cooked bundle.
	const auto AddRoot = [&Roots](const FString& ContentDir) {
		if (!ContentDir.IsEmpty())
		{
			const FString Root = Private::ToDevUIRoot(ContentDir);
			if (!Roots.ContainsByPredicate([&Root](const FString& Existing) {
					return Existing.Equals(Root, ESearchCase::CaseSensitive);
				}))
			{
				Roots.Add(Root);
			}
		}
	};

	AddRoot(VaCuusContentDir);
	for (const FString& ContentDir : OtherPluginContentDirs)
	{
		AddRoot(ContentDir);
	}
	AddRoot(ProjectContentDir);

	return Roots;
}

const TArray<FString>& GetDocumentRoots()
{
	// Function-local static: initialized exactly once, thread-safely, and primed from
	// FVaCuusModule::StartupModule() so the first call is on the game thread.
	static const TArray<FString> Roots = Private::BuildDocumentRoots();
	return Roots;
}

FString ResolveExistingDocument(const FString& VfsPath, FString* OutRoot, bool bIncludeMountedBundles)
{
	if (OutRoot)
	{
		OutRoot->Reset();
	}

	if (VfsPath.IsEmpty())
	{
		return FString();
	}

	if (!FPaths::IsRelative(VfsPath))
	{
		// Absolute passthrough: no root is involved, so there is nothing to report in
		// OutRoot either.
		return FPaths::FileExists(VfsPath) ? VfsPath : FString();
	}

	if (bIncludeMountedBundles)
	{
		// Bundle-first, matching FVaCuusFileInterface::Open exactly (see the header):
		// a caller that existence-checks here and then opens must get the same answer
		// twice. The pseudo-path is deliberately unopenable -- it names the serving
		// bundle for logs, nothing more.
		FString BundleName;
		if (FVaCuusBundleMountTable::ContainsPath(VfsPath, &BundleName))
		{
			const FString BundleRoot = FString::Printf(TEXT("bundle://%s"), *BundleName);
			if (OutRoot)
			{
				*OutRoot = BundleRoot;
			}
			return BundleRoot / VaCuusBundleFormat::NormalizePath(VfsPath);
		}
	}

	for (const FString& Root : GetDocumentRoots())
	{
		const FString Candidate = Root / VfsPath;
		if (FPaths::FileExists(Candidate))
		{
			if (OutRoot)
			{
				*OutRoot = Root;
			}
			return Candidate;
		}
	}

	return FString();
}

int32 ScanShadowedDocuments(const TArray<FString>& Roots, TArray<FString>* OutShadowedPaths)
{
	if (OutShadowedPaths)
	{
		OutShadowedPaths->Reset();
	}

	IFileManager& FileManager = IFileManager::Get();

	// Predict the pack's winner. Loose opens preserve the requested spelling, so
	// case-only variants can still be read separately on a case-sensitive volume.
	TMap<FString, FString> ClaimedBy;
	int32 NumShadowed = 0;

	for (const FString& Root : Roots)
	{
		const FString FullRoot = FPaths::ConvertRelativePathToFull(Root);

		TArray<FString> Found;
		for (const TCHAR* Extension : VaCuusBundleFormat::GetPackedExtensions())
		{
			// bClearFileNames false: accumulate across extensions, one pass per extension,
			// exactly as VaCuusBundlePack::EnumerateTree walks the same tree.
			FileManager.FindFilesRecursive(Found, *FullRoot, *(FString(TEXT("*.")) + Extension),
				/*Files*/ true, /*Directories*/ false, /*bClearFileNames*/ false);
		}

		// Sorted so that WHICH of two case-only variants inside one root is reported as the
		// winner is a property of the names and not of the filesystem's enumeration order.
		// Strcmp for the reason DiscoverPluginRoots states: an insensitive compare answers
		// "equal" for precisely the pair that can collide here.
		Found.Sort([](const FString& A, const FString& B) { return FCString::Strcmp(*A, *B) < 0; });

		for (const FString& DiskPath : Found)
		{
			const FString FullPath = FPaths::ConvertRelativePathToFull(DiskPath);
			if (!FullPath.StartsWith(FullRoot + TEXT("/")))
			{
				// Defensive, and the same guard EnumerateTree carries: a path that does not
				// sit under the root it was found through cannot be turned into a relative
				// VFS path, and silently mangling one would make this report lie.
				UE_LOG(LogVaCuus, Warning, TEXT("Ignoring '%s': it is not under the root '%s' it was found through"),
					*FullPath, *FullRoot);
				continue;
			}

			const FString NormalizedPath = VaCuusBundleFormat::NormalizePath(FullPath.Mid(FullRoot.Len() + 1));
			if (VaCuusBundleFormat::IsExcludedTestPath(NormalizedPath))
			{
				// Automation fixtures are per-root by design (every root may carry its own
				// Tests/), never shipped, and never addressed by a document -- so a
				// collision between two of them is not a fault to report.
				continue;
			}

			if (const FString* Winner = ClaimedBy.Find(NormalizedPath))
			{
				UE_LOG(LogVaCuus, Warning,
					TEXT("Bundle path '%s' has a collision: '%s' wins when packed; '%s' is excluded from the bundle. ")
					TEXT("Loose-file lookup can differ, especially for case-only names. Rename one copy to pack both"),
					*NormalizedPath, **Winner, *FullPath);

				++NumShadowed;
				if (OutShadowedPaths)
				{
					OutShadowedPaths->Add(NormalizedPath);
				}
				continue;
			}

			ClaimedBy.Add(NormalizedPath, FullPath);
		}
	}

	// Logged at Log even on zero: it is the line that proves the scan RAN. Without it, a tree
	// with no duplicates and a scan that silently found no directories to walk at all read
	// identically -- and the second is what a regression in the root list would look like.
	UE_LOG(LogVaCuus, Log, TEXT("VaCuus document shadow scan: %d file(s) excluded by bundle-path collisions (%d total)"),
		NumShadowed, ClaimedBy.Num() + NumShadowed);

	return NumShadowed;
}

namespace Private
{
/**
 * Enough bytes for the longest thing we test for. The Git-LFS v1 pointer's first line is
 * 42 characters (`version https://git-lfs.github.com/spec/v1`); the image signatures are
 * 8 and 3. 64 keeps the read to one block with room to spare.
 */
static constexpr int32 GImageProbeBytes = 64;

/**
 * The v1 pointer's MANDATORY first line, verbatim from the spec
 * (https://github.com/git-lfs/git-lfs/blob/main/docs/spec.md): the `version` key comes
 * first and its value is this exact URL. Matching the line rather than merely "starts
 * with `version`" is what keeps a text file that happens to begin with that word from
 * being reported as a pointer.
 */
static const ANSICHAR* GLfsPointerFirstLine = "version https://git-lfs.github.com/spec/v1";

/** PNG signature, RFC 2083 section 3.1 -- the first eight bytes of every PNG datastream. */
static constexpr uint8 GPngSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

/** JPEG SOI followed by the first marker's 0xFF -- true of JFIF and Exif alike. */
static constexpr uint8 GJpegSignature[] = {0xFF, 0xD8, 0xFF};

static bool HeadMatches(const uint8* Head, int32 NumRead, const uint8* Signature, int32 SignatureLen)
{
	return NumRead >= SignatureLen && FMemory::Memcmp(Head, Signature, SignatureLen) == 0;
}

/**
 * The first bytes a MOUNTED bundle serves for VfsPath, or false when no mount carries it.
 *
 * The bundle's own pseudo-path cannot go through the platform file layer (see
 * ResolveExistingDocument), so the span is read directly. Precedence is not re-derived
 * here: ContainsPath already answered WHICH bundle serves the path under the first-hit-
 * wins rule, and this only finds that record's entry.
 */
static bool ReadBundleHead(const FString& VfsPath, uint8* Head, int32& OutNumRead)
{
	FString BundleName;
	if (!FVaCuusBundleMountTable::ContainsPath(VfsPath, &BundleName))
	{
		return false;
	}

	const TSharedPtr<const FVaCuusBundleLookup> Lookup = FVaCuusBundleMountTable::GetLookup();
	if (!Lookup)
	{
		return false;
	}

	const FString NormalizedPath = VaCuusBundleFormat::NormalizePath(VfsPath);
	for (const TSharedRef<FVaCuusBundleMount>& Mount : Lookup->Mounts)
	{
		if (Mount->BundleName != BundleName)
		{
			continue;
		}
		const FVaCuusBundleEntry* Entry = Mount->FindEntry(NormalizedPath);
		if (!Entry || !Mount->Base)
		{
			return false;
		}
		OutNumRead = static_cast<int32>(FMath::Min<int64>(Entry->Size, GImageProbeBytes));
		if (OutNumRead > 0)
		{
			FMemory::Memcpy(Head, Mount->Base + Entry->Offset, OutNumRead);
		}
		return true;
	}

	return false;
}
}	 // namespace Private

EVaCuusImageProbe ProbeImage(const FString& VfsPath, FString* OutDiagnosis)
{
	if (OutDiagnosis)
	{
		OutDiagnosis->Reset();
	}

	const auto Diagnose = [OutDiagnosis](EVaCuusImageProbe Result, FString&& Sentence) {
		if (OutDiagnosis)
		{
			*OutDiagnosis = MoveTemp(Sentence);
		}
		return Result;
	};

	FString Root;
	const FString ResolvedPath = ResolveExistingDocument(VfsPath, &Root);
	if (ResolvedPath.IsEmpty())
	{
		return Diagnose(EVaCuusImageProbe::Missing,
			FString::Printf(
				TEXT("'%s' is not served by any DevUI root (%s) and no mounted bundle carries it"),
				*VfsPath, *FString::Join(GetDocumentRoots(), TEXT(" | "))));
	}

	uint8 Head[Private::GImageProbeBytes] = {};
	int32 NumRead = 0;
	bool bRead = false;

	if (Root.StartsWith(TEXT("bundle://")))
	{
		bRead = Private::ReadBundleHead(VfsPath, Head, NumRead);
	}
	else if (const TUniquePtr<FArchive> Reader{IFileManager::Get().CreateFileReader(*ResolvedPath)})
	{
		NumRead = static_cast<int32>(FMath::Min<int64>(Reader->TotalSize(), Private::GImageProbeBytes));
		if (NumRead > 0)
		{
			Reader->Serialize(Head, NumRead);
		}
		bRead = !Reader->IsError();
	}

	if (!bRead)
	{
		return Diagnose(EVaCuusImageProbe::Unreadable,
			FString::Printf(TEXT("'%s' resolves to '%s' but its bytes could not be read"), *VfsPath, *ResolvedPath));
	}

	if (Private::HeadMatches(Head, NumRead, Private::GPngSignature, UE_ARRAY_COUNT(Private::GPngSignature)) ||
		Private::HeadMatches(Head, NumRead, Private::GJpegSignature, UE_ARRAY_COUNT(Private::GJpegSignature)))
	{
		return EVaCuusImageProbe::Ok;
	}

	const int32 PointerLen = FCStringAnsi::Strlen(Private::GLfsPointerFirstLine);
	if (NumRead >= PointerLen && FMemory::Memcmp(Head, Private::GLfsPointerFirstLine, PointerLen) == 0)
	{
		return Diagnose(EVaCuusImageProbe::GitLfsPointer,
			FString::Printf(
				TEXT("'%s' (%s) is a Git-LFS POINTER, not an image -- this tree was cloned without git-lfs ")
				TEXT("installed, so every LFS-tracked file is a stub. Run `git lfs pull` to fetch the real bytes"),
				*VfsPath, *ResolvedPath));
	}

	return Diagnose(EVaCuusImageProbe::NotAnImage,
		FString::Printf(
			TEXT("'%s' (%s) begins with neither a PNG nor a JPEG signature and is not a Git-LFS pointer ")
			TEXT("either -- it is truncated, corrupt, or not the file it claims to be"),
			*VfsPath, *ResolvedPath));
}
}	 // namespace VaCuusContentPaths
