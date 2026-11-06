// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDataVariable.h"
#include "VaCuusEngine.h"
#include "VaCuusModelLayout.h"
#include "VaCuusModelShadow.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusTestLogCapture.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "VaCuusModelLayoutTestTypes.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"

#include <atomic>

#include <RmlUi/Core.h>
#include <RmlUi/Core/DataModelHandle.h>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE SILENT FAILURES OF THE 2026-10-06 FIELD REPORT, MADE LOUD (epic VaCuus-w87). Every test
 * here is a document that used to render something plausible -- an empty card row, the word
 * "Text", a hidden card's colour, a bold title in regular weight -- while the log said at most
 * a generic Warning. Each one now pins the diagnostic or the behaviour that replaced the
 * silence.
 *
 * One rig for all of them: a real Rml::Context on the real UI thread, a model bound straight
 * from a layout and a shadow (the VaCuus.Model.ArrayBinding shape, generalised over the struct
 * type), and numbered phases whose observations are written on the UI thread and read on the
 * test thread after the phase counter's release/acquire hand-off.
 */
namespace VaCuusBindDiagnosticsTest
{
class FBindProbeHost final : public FVaCuusTestDocumentHost
{
public:
	/** InType may be null: a model-less document (the dp and font tests need no binding). */
	FBindProbeHost(const UScriptStruct* InType, const char* InModelName)
		: FVaCuusTestDocumentHost(TEXT("vacuus_bind_diag"), "vacuus://bind_diag.rml", Rml::FocusFlag::Document)
		, Type(InType)
		, ModelName(InModelName)
	{
	}

	/** The bind happens HERE, before any load: `data-model` is read exactly once, in
	 *  Element::SetParent, so a model created after the document parsed attaches to nothing. */
	virtual bool OnInitialized() override
	{
		if (Type == nullptr)
		{
			return true;
		}

		Layout = FVaCuusModelLayout(Type);
		Shadow = FVaCuusModelShadow(Type);
		if (Seed)
		{
			Seed(Shadow.GetData());
		}
		if (OnBuilt)
		{
			OnBuilt(*this);
		}

		Rml::DataModelConstructor Constructor = Context->CreateDataModel(ModelName);
		if (!Constructor)
		{
			return false;
		}

		ModelHandle = Constructor.GetModelHandle();
		NumBound = VaCuusData::BindModelVariables(Constructor, Layout, Shadow);
		return true;
	}

	/** After the context is gone: RmlUi holds a raw pointer into the shadow, with no unbind. */
	virtual void OnShutdown() override { Shadow.Reset(); }

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		const int32 Requested = RequestedPhase.load(std::memory_order_acquire);
		if (Requested > CompletedPhase.load(std::memory_order_relaxed))
		{
			if (Actions.IsValidIndex(Requested) && Actions[Requested])
			{
				Actions[Requested](*this);
			}
			Context->Update();
			if (Observe)
			{
				Observe(*this, Requested);
			}
			CompletedPhase.store(Requested, std::memory_order_release);
		}
		else
		{
			Context->Update();
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ UI-thread helpers for actions and observers.

	Rml::Element* Find(const char* Id) const
	{
		check(FVaCuusUIThread::IsInUIThread());
		return RmlDocument != nullptr ? RmlDocument->GetElementById(Id) : nullptr;
	}

	/** The generated rows under ContainerId, in order; the data-for template keeps its attribute. */
	TArray<FString> RowTexts(const char* ContainerId) const
	{
		TArray<FString> Out;
		if (Rml::Element* Container = Find(ContainerId))
		{
			for (int Index = 0; Index < Container->GetNumChildren(); ++Index)
			{
				Rml::Element* Child = Container->GetChild(Index);
				if (Child != nullptr && !Child->HasAttribute("data-for"))
				{
					Out.Add(FString(UTF8_TO_TCHAR(Child->GetInnerRML().c_str())));
				}
			}
		}
		return Out;
	}

	template <typename T>
	T& Model()
	{
		check(FVaCuusUIThread::IsInUIThread());
		return *static_cast<T*>(Shadow.GetData());
	}

	void Dirty(const char* Name) { ModelHandle.DirtyVariable(Name); }

	//~ Configured on the test thread BEFORE EnqueueAddView; immutable after.

	TUniqueFunction<void(void*)> Seed;
	TUniqueFunction<void(FBindProbeHost&)> OnBuilt;
	TArray<TUniqueFunction<void(FBindProbeHost&)>> Actions;
	TUniqueFunction<void(FBindProbeHost&, int32)> Observe;

	std::atomic<int32> RequestedPhase{0};
	std::atomic<int32> CompletedPhase{-1};

	//~ UI-thread state, readable after a completed phase.

	FVaCuusModelLayout Layout;
	int32 NumBound = 0;

private:
	const UScriptStruct* Type = nullptr;
	const char* ModelName = nullptr;
	FVaCuusModelShadow Shadow;
	Rml::DataModelHandle ModelHandle;
};

static bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
{
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const uint64 Before = UIThread.GetFrameCount();
		UIThread.Trigger();
		if (!UIThread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}
	return true;
}

static bool RunPhase(FVaCuusUIThread& UIThread, FBindProbeHost& Host, int32 Phase)
{
	Host.RequestedPhase.store(Phase, std::memory_order_release);

	const double Deadline = FPlatformTime::Seconds() + 10.0;
	while (Host.CompletedPhase.load(std::memory_order_acquire) < Phase)
	{
		if (FPlatformTime::Seconds() > Deadline || !RunFrames(UIThread, 1))
		{
			return false;
		}
	}
	return true;
}

/** Adds the view, loads Document and runs phase 0. Returns the view id, or 0 on failure. */
static uint32 StartView(FAutomationTestBase& Test, FVaCuusUIThread& UIThread, TUniquePtr<FBindProbeHost> OwnedHost,
	const TSharedRef<FVaCuusViewStatus>& Status, const TCHAR* Document)
{
	FBindProbeHost& Host = *OwnedHost;
	const uint32 ViewId = UIThread.AllocateViewId();
	UIThread.EnqueueAddView(ViewId, MoveTemp(OwnedHost), FIntPoint(400, 300), Status);
	UIThread.EnqueueLoadDocumentFromMemory(ViewId, Document, /*LoadSerial=*/1);

	if (!Test.TestTrue(TEXT("the initial phase ran"), RunPhase(UIThread, Host, 0))
		|| !Test.TestTrue(TEXT("the document loaded"),
			Status->LoadCompletedSerial.load(std::memory_order_acquire) == 1
				&& Status->LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded)))
	{
		return 0;
	}
	return ViewId;
}

static bool Preflight(FAutomationTestBase& Test)
{
	if (!FPlatformProcess::SupportsMultithreading())
	{
		Test.AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return false;
	}
	return Test.TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized());
}
}	 // namespace VaCuusBindDiagnosticsTest

/**
 * A ROW WITH A CONTAINER MEMBER STILL RENDERS ITS ROWS (bead VaCuus-w87.4). The field report's
 * chest gave each offer card its own TArray of trait lines; the whole Offers array was refused
 * for it, the card row rendered empty, and every test was green. Now the container member is
 * refused alone, with one Warning naming the array and the member, and the rest binds.
 *
 * THE SAME ROW TYPE IS ALSO A VALID ROOT, where its TArray member binds as an ordinary top-level
 * array -- so the definition registry, keyed on the struct, must not hand the element's pruned
 * definitions to a root or the root's full ones to an element. The OnBuilt probe takes both and
 * asserts they are two sets.
 *
 * RESTORE-THE-BUG: route an element build's container back to the whole-array refusal and the
 * rows read empty; key the registry on the struct alone and the two lookups return one set.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusNestedContainerRowTest, "VaCuus.Model.NestedContainerRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusNestedContainerRowTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBindDiagnosticsTest;

	if (!Preflight(*this))
	{
		return true;
	}

	// Once: the probe builds the owner layout once (the bind's own), and the row-as-root layout
	// refuses nothing.
	AddExpectedMessagePlain(TEXT("array property 'Rows' (TArray): element member 'Inner' (TArray) is not bound"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	TUniquePtr<FBindProbeHost> OwnedHost = MakeUnique<FBindProbeHost>(FVaCuusNestedRowModel::StaticStruct(), "nest");
	FBindProbeHost* Host = OwnedHost.Get();

	Host->Seed = [](void* Data)
	{
		FVaCuusNestedRowModel& Model = *static_cast<FVaCuusNestedRowModel*>(Data);
		Model.Rows.SetNum(2);
		Model.Rows[0].Kept = TEXT("first");
		Model.Rows[0].Inner = {1, 2, 3};
		Model.Rows[1].Kept = TEXT("second");
	};

	bool bRootHasInner = false;
	bool bElementLacksInner = false;
	bool bTwoDefinitionSets = false;
	Host->OnBuilt = [&bRootHasInner, &bElementLacksInner, &bTwoDefinitionSets](FBindProbeHost& Host)
	{
		const FVaCuusModelLayout RowAsRoot(FVaCuusTestNestedArrayRow::StaticStruct());
		bRootHasInner = RowAsRoot.FindField(TEXT("Inner")) != nullptr;

		const FVaCuusModelField* Rows = Host.Layout.FindField(TEXT("Rows"));
		if (Rows == nullptr || Rows->ArrayDesc == nullptr || !Rows->ArrayDesc->IsStructElement())
		{
			return;
		}
		bElementLacksInner = Rows->ArrayDesc->ElementLayout->FindField(TEXT("Inner")) == nullptr;

		const FVaCuusModelDefinitions* RootSet = FVaCuusDefinitionRegistry::GetOrCreate(RowAsRoot);
		const FVaCuusModelDefinitions* ElementSet = FVaCuusDefinitionRegistry::GetOrCreate(*Rows->ArrayDesc->ElementLayout);
		bTwoDefinitionSets = RootSet != nullptr && ElementSet != nullptr && RootSet != ElementSet;
	};

	TArray<FString> Rows;
	Host->Observe = [&Rows](FBindProbeHost& Host, int32 /*Phase*/) { Rows = Host.RowTexts("rows"); };

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = StartView(*this, *UIThread, MoveTemp(OwnedHost), Status, TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="nest">
	<div id="rows"><div data-for="r : Rows">{{ r.Kept }}</div></div>
</body>
</rml>)"));
	if (ViewId == 0)
	{
		return false;
	}

	if (TestEqual(TEXT("both rows render although the row type carries a container"), Rows.Num(), 2))
	{
		TestEqual(TEXT("row 0's bound member"), Rows[0], FString(TEXT("first")));
		TestEqual(TEXT("row 1's bound member"), Rows[1], FString(TEXT("second")));
	}
	TestTrue(TEXT("as a ROOT the row type binds its container"), bRootHasInner);
	TestTrue(TEXT("as an ELEMENT it does not"), bElementLacksInner);
	TestTrue(TEXT("so the registry keeps two definition sets for the one struct"), bTwoDefinitionSets);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);
	return true;
}

/**
 * A data-for ALIAS THAT A TOP-LEVEL VARIABLE SHADOWS IS AN ERROR THAT NAMES THE ELEMENT (bead
 * VaCuus-w87.5). RmlUi resolves a top-level variable before any alias (DataModel.cpp's
 * ResolveAddress), so such an alias is unreachable: in the field report `data-for="t :
 * OfferedTraits"` read VaCuus's reserved translation variable and printed the key fallback
 * "Text" on every card. The only signal was a generic Warning naming neither document nor
 * element. Both spellings are probed: `t`, which every VaCuus model binds, and an ordinary
 * field name.
 *
 * RESTORE-THE-BUG: drop the shadow check from DataViewFor::Initialize and both expectations go
 * unmet (and RmlUi's generic per-row Warning does not come back: InsertAlias now reports only a
 * placed element, i.e. the data-alias path).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusAliasShadowedTest, "VaCuus.Model.AliasShadowed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusAliasShadowedTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBindDiagnosticsTest;

	if (!Preflight(*this))
	{
		return true;
	}

	// ONCE PER data-for, not once per row: each row's InsertAlias runs before the row is parented
	// (DataViewDefault.cpp:560-563), with no address or document to name, so the template's
	// Initialize reports it instead -- where the element is placed and the document known.
	AddExpectedMessagePlain(TEXT("Alias 't' on div"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("Alias 'Label' on div"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	TUniquePtr<FBindProbeHost> OwnedHost = MakeUnique<FBindProbeHost>(FVaCuusBindDiagnosticsModel::StaticStruct(), "diag");
	FBindProbeHost* Host = OwnedHost.Get();
	Host->Seed = [](void* Data)
	{
		FVaCuusBindDiagnosticsModel& Model = *static_cast<FVaCuusBindDiagnosticsModel*>(Data);
		Model.Colour = TEXT("#ff0000");
		Model.Label = TEXT("top-level");
		Model.Lines = {TEXT("one"), TEXT("two")};
	};

	FVaCuusTestLogCapture LogCapture;

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = StartView(*this, *UIThread, MoveTemp(OwnedHost), Status, TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="diag">
	<div id="trows"><div data-for="t : Lines">{{ t }}</div></div>
	<div id="lrows"><div data-for="Label : Lines">{{ Label }}</div></div>
</body>
</rml>)"));
	if (ViewId == 0)
	{
		return false;
	}

	// The Error names the document as well as the element, so a reader with four documents open
	// knows which one to fix.
	TestEqual(TEXT("the Error names the document"), LogCapture.Count(TEXT("in 'vacuus://bind_diag.rml'")), 2);
	TestEqual(TEXT("and the template element's place in it"), LogCapture.Count(TEXT("on div < div#trows < body")), 1);
	TestEqual(TEXT("and RmlUi's generic Warning no longer stands in for it"),
		LogCapture.Count(TEXT("is shadowed by a global variable")), 0);

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);
	return true;
}

/**
 * AN EMPTY STRING IN A STYLE BINDING REMOVES THE PROPERTY (bead VaCuus-w87.8). The field
 * report's comparison card kept its colour fields at "" while hidden; bindings under a false
 * data-if keep evaluating, so every update wrote `color: ;`, which fails to parse and warned
 * with no element named. CSSOM's precedent is that assigning '' to an inline style property
 * removes it, and that is what DataViewStyle now does. A MALFORMED non-empty value still warns
 * -- and the Warning now names the element.
 *
 * RESTORE-THE-BUG: drop the empty-value branch from DataViewStyle::Update and phase 0 logs the
 * parse Warning (the capture count reads 1) while phase 2 leaves the red in place.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusEmptyStyleValueTest, "VaCuus.Model.EmptyStyleValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusEmptyStyleValueTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBindDiagnosticsTest;

	if (!Preflight(*this))
	{
		return true;
	}

	// Phase 3's malformed value: still a Warning, now with the element in it.
	AddExpectedMessagePlain(TEXT("'color: notacolour%;' on div#swatch"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	TUniquePtr<FBindProbeHost> OwnedHost = MakeUnique<FBindProbeHost>(FVaCuusBindDiagnosticsModel::StaticStruct(), "diag");
	FBindProbeHost* Host = OwnedHost.Get();

	// Phase 0: Colour starts "" (the default-constructed field). Phase 1: a real colour.
	// Phase 2: back to "". Phase 3: a malformed value.
	Host->Actions.SetNum(4);
	Host->Actions[1] = [](FBindProbeHost& Host)
	{
		Host.Model<FVaCuusBindDiagnosticsModel>().Colour = TEXT("#ff0000");
		Host.Dirty("Colour");
	};
	Host->Actions[2] = [](FBindProbeHost& Host)
	{
		Host.Model<FVaCuusBindDiagnosticsModel>().Colour.Reset();
		Host.Dirty("Colour");
	};
	Host->Actions[3] = [](FBindProbeHost& Host)
	{
		Host.Model<FVaCuusBindDiagnosticsModel>().Colour = TEXT("notacolour%");
		Host.Dirty("Colour");
	};

	bool bHasLocalColour[4] = {};
	Host->Observe = [&bHasLocalColour](FBindProbeHost& Host, int32 Phase)
	{
		Rml::Element* Swatch = Host.Find("swatch");
		bHasLocalColour[Phase] = Swatch != nullptr && Swatch->GetLocalProperty("color") != nullptr;
	};

	FVaCuusTestLogCapture LogCapture;

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = StartView(*this, *UIThread, MoveTemp(OwnedHost), Status, TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="diag">
	<div id="swatch" data-style-color="Colour">x</div>
</body>
</rml>)"));
	if (ViewId == 0)
	{
		return false;
	}

	TestFalse(TEXT("an empty value sets no inline colour"), bHasLocalColour[0]);
	TestEqual(TEXT("and logs no parse Warning"), LogCapture.Count(TEXT("Syntax error parsing inline property declaration 'color: ;'")), 0);

	if (TestTrue(TEXT("phase 1 ran"), RunPhase(*UIThread, *Host, 1)))
	{
		TestTrue(TEXT("a real value sets it"), bHasLocalColour[1]);
	}
	if (TestTrue(TEXT("phase 2 ran"), RunPhase(*UIThread, *Host, 2)))
	{
		TestFalse(TEXT("and an empty value REMOVES it again -- the stale red must not stay"), bHasLocalColour[2]);
	}
	TestEqual(TEXT("still no parse Warning for either empty write"),
		LogCapture.Count(TEXT("Syntax error parsing inline property declaration 'color: ;'")), 0);
	TestTrue(TEXT("phase 3 ran"), RunPhase(*UIThread, *Host, 3));

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);
	return true;
}

/**
 * dp FOLLOWS THE VIEW'S DPI SCALE (bead VaCuus-w87.12, owner decision 2026-10-06). The view is
 * laid out in PHYSICAL pixels, and VaCuus never set RmlUi's dp ratio, so `dp` meant `px` and a
 * document authored in dp -- vacuus-base.rcss, the CLI template, M5Hud -- came out half size at
 * 4K. The ratio now rides the Resize command; SVaCuusWidget sends Slate's geometry scale (the
 * project's DPI curve), and `px` is untouched.
 *
 * RESTORE-THE-BUG: drop the SetDensityIndependentPixelRatio call from the UI thread's resize
 * handling and the dp box measures 50, not 100.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusDpRatioTest, "VaCuus.View.DpRatio",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusDpRatioTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBindDiagnosticsTest;

	if (!Preflight(*this))
	{
		return true;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	TUniquePtr<FBindProbeHost> OwnedHost = MakeUnique<FBindProbeHost>(nullptr, nullptr);
	FBindProbeHost* Host = OwnedHost.Get();

	float DpBoxWidth[2] = {};
	float PxBoxWidth[2] = {};
	float ContextRatio[2] = {};
	Host->Observe = [&DpBoxWidth, &PxBoxWidth, &ContextRatio](FBindProbeHost& Host, int32 Phase)
	{
		if (Rml::Element* DpBox = Host.Find("dpbox"))
		{
			DpBoxWidth[Phase] = DpBox->GetBox().GetSize().x;
		}
		if (Rml::Element* PxBox = Host.Find("pxbox"))
		{
			PxBoxWidth[Phase] = PxBox->GetBox().GetSize().x;
		}
		ContextRatio[Phase] = Host.GetContext()->GetDensityIndependentPixelRatio();
	};

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = StartView(*this, *UIThread, MoveTemp(OwnedHost), Status, TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; height: 10px; }</style></head>
<body>
	<div id="dpbox" style="width: 50dp;"/>
	<div id="pxbox" style="width: 50px;"/>
</body>
</rml>)"));
	if (ViewId == 0)
	{
		return false;
	}

	TestEqual(TEXT("a fresh view lays dp out at ratio 1"), DpBoxWidth[0], 50.0f);

	// Same size, a new ratio: the resize must not be dropped as "unchanged".
	UIThread->EnqueueResize(ViewId, FIntPoint(400, 300), /*DpRatio=*/2.0f);
	if (TestTrue(TEXT("phase 1 ran"), RunPhase(*UIThread, *Host, 1)))
	{
		TestEqual(TEXT("the context took the ratio"), ContextRatio[1], 2.0f);
		TestEqual(TEXT("a 50dp box is 100 physical pixels at ratio 2"), DpBoxWidth[1], 100.0f);
		TestEqual(TEXT("and px is untouched"), PxBoxWidth[1], 50.0f);
	}

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);
	return true;
}

/**
 * A REQUESTED WEIGHT WITH NO FACE SAYS SO, ONCE (bead VaCuus-w87.13, owner decision
 * 2026-10-06: a Warning, no bold face shipped). The plugin loads one face, LatoLatin-Regular,
 * and RmlUi's FontFamily::GetFaceHandle silently draws the nearest weight -- so every
 * `font-weight: bold` title in the field report drew regular, with nothing logged.
 *
 * RESTORE-THE-BUG: drop the Warning from FontFamily::GetFaceHandle and the expectation is unmet;
 * drop its once-per latch and the count reads 2.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontMissingWeightTest, "VaCuus.Font.MissingWeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontMissingWeightTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBindDiagnosticsTest;

	if (!Preflight(*this))
	{
		return true;
	}

	// Once, although two elements ask for bold; the regular paragraph asks for nothing missing.
	// Lowercase because that is the key RmlUi files every family under (FontProvider.cpp:247-256).
	AddExpectedMessagePlain(TEXT("Font family 'latolatin' has no normal face of weight 700"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = StartView(*this, *UIThread, MakeUnique<FBindProbeHost>(nullptr, nullptr), Status, TEXT(R"(<rml>
<head><style>
body { display: block; font-family: LatoLatin; font-size: 16px; }
p { display: block; }
.title { font-weight: bold; }
</style></head>
<body>
	<p class="title">first title</p>
	<p class="title">second title</p>
	<p>regular body text</p>
</body>
</rml>)"));
	if (ViewId == 0)
	{
		return false;
	}

	UIThread->EnqueueRemoveView(ViewId);
	RunFrames(*UIThread, 1);
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
