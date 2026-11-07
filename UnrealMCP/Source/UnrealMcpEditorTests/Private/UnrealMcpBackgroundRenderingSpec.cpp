// Copyright (c) 2026 Ivan Murzak. Licensed under the Apache License, Version 2.0.
// See the LICENSE file in the repository root for more information.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorPerformanceSettings.h"
#include "UObject/UnrealType.h"
#include "Session/UnrealMcpBackgroundRendering.h"

/**
 * Background rendering (docs/ARCHITECTURE.md §10): when the hold applies, that it lifts both background throttles and
 * puts the user's "Use Less CPU when in Background" value back, and that an edit of the settings object while held
 * saves the user's value, never the override. Rendering itself is live-verified windowed (background and minimized).
 */
BEGIN_DEFINE_SPEC(FUnrealMcpBackgroundRenderingSpec, "UnrealMcp.BackgroundRendering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	bool BgRenderSpecSavedThrottle = true;

	UEditorPerformanceSettings* BgRenderSpecSettings() const { return GetMutableDefault<UEditorPerformanceSettings>(); }

	void BgRenderSpecSetThrottle(bool bValue) const { BgRenderSpecSettings()->bThrottleCPUWhenNotForeground = bValue; }

	bool BgRenderSpecThrottle() const { return BgRenderSpecSettings()->bThrottleCPUWhenNotForeground != 0; }

	int32 BgRenderSpecDelegateCount() const { return GEditor ? GEditor->ShouldDisableCPUThrottlingDelegates.Num() : 0; }

	FPropertyChangedEvent BgRenderSpecEvent(const TCHAR* PropertyName) const
	{
		FProperty* Property = PropertyName ? UEditorPerformanceSettings::StaticClass()->FindPropertyByName(PropertyName) : nullptr;
		return FPropertyChangedEvent(Property);
	}

END_DEFINE_SPEC(FUnrealMcpBackgroundRenderingSpec)

void FUnrealMcpBackgroundRenderingSpec::Define()
{
	Describe("keep-awake policy", [this]()
	{
		It("holds while a client is connected or a call is in flight, and not otherwise", [this]()
		{
			FUnrealMcpKeepAwakePolicy Policy;
			TestFalse(TEXT("nothing connected, no calls"), Policy.ShouldHold(10.0, false, 0, 0));
			TestTrue(TEXT("a connected client"), Policy.ShouldHold(11.0, true, 0, 0));
			TestTrue(TEXT("a call in flight"), Policy.ShouldHold(12.0, false, 0, 1));
		});

		It("keeps holding for the grace period after the last finished call", [this]()
		{
			FUnrealMcpKeepAwakePolicy Policy;
			const double Grace = FUnrealMcpKeepAwakePolicy::ActivityGraceSeconds;
			TestFalse(TEXT("idle start"), Policy.ShouldHold(100.0, false, 0, 0));
			TestTrue(TEXT("a call finished (counter moved)"), Policy.ShouldHold(101.0, false, 1, 0));
			TestTrue(TEXT("inside the grace"), Policy.ShouldHold(101.0 + Grace - 1.0, false, 1, 0));
			TestFalse(TEXT("after the grace"), Policy.ShouldHold(101.0 + Grace + 1.0, false, 1, 0));
			TestTrue(TEXT("a new call restarts it"), Policy.ShouldHold(500.0, false, 2, 0));
		});
	});

	Describe("hold", [this]()
	{
		BeforeEach([this]() { BgRenderSpecSavedThrottle = BgRenderSpecThrottle(); });
		AfterEach([this]() { BgRenderSpecSetThrottle(BgRenderSpecSavedThrottle); });

		It("lifts both throttles while held and restores the user's value on release", [this]()
		{
			if (!GEditor)
				return;
			BgRenderSpecSetThrottle(true);
			const int32 Delegates = BgRenderSpecDelegateCount();
			FUnrealMcpBackgroundRendering Rendering;

			Rendering.SetHeld(true);
			TestTrue(TEXT("held"), Rendering.IsHeld());
			TestFalse(TEXT("the realtime override setting is off in memory"), BgRenderSpecThrottle());
			TestEqual(TEXT("one throttle-disable delegate added"), BgRenderSpecDelegateCount(), Delegates + 1);
			TestTrue(TEXT("the user's value is kept"), Rendering.GetUserThrottleSetting());

			Rendering.SetHeld(false);
			TestTrue(TEXT("the user's value is back"), BgRenderSpecThrottle());
			TestEqual(TEXT("our delegate is gone"), BgRenderSpecDelegateCount(), Delegates);
		});

		It("leaves a user who turned the throttle off with it off", [this]()
		{
			if (!GEditor)
				return;
			BgRenderSpecSetThrottle(false);
			FUnrealMcpBackgroundRendering Rendering;
			Rendering.SetHeld(true);
			Rendering.SetHeld(false);
			TestFalse(TEXT("still off"), BgRenderSpecThrottle());
		});

		It("releases on Shutdown and on destruction", [this]()
		{
			if (!GEditor)
				return;
			BgRenderSpecSetThrottle(true);
			const int32 Delegates = BgRenderSpecDelegateCount();
			{
				FUnrealMcpBackgroundRendering Rendering;
				Rendering.SetHeld(true);
			}
			TestTrue(TEXT("restored by the destructor"), BgRenderSpecThrottle());
			TestEqual(TEXT("delegate removed by the destructor"), BgRenderSpecDelegateCount(), Delegates);

			FUnrealMcpBackgroundRendering Rendering;
			Rendering.SetHeld(true);
			Rendering.Shutdown();
			TestFalse(TEXT("not held after Shutdown"), Rendering.IsHeld());
			TestTrue(TEXT("restored by Shutdown"), BgRenderSpecThrottle());
		});
	});

	Describe("settings edits while held (the saved preference never takes the override)", [this]()
	{
		BeforeEach([this]() { BgRenderSpecSavedThrottle = BgRenderSpecThrottle(); });
		AfterEach([this]() { BgRenderSpecSetThrottle(BgRenderSpecSavedThrottle); });

		It("an edit of another property puts the user's value in memory for the save, then Reconcile re-applies", [this]()
		{
			if (!GEditor)
				return;
			BgRenderSpecSetThrottle(true);
			FUnrealMcpBackgroundRendering Rendering;
			Rendering.SetHeld(true);

			FPropertyChangedEvent Event = BgRenderSpecEvent(TEXT("bShowFrameRateAndMemory"));
			Rendering.HandleSettingChanged(BgRenderSpecSettings(), Event);
			TestTrue(TEXT("the save that follows writes the user's value"), BgRenderSpecThrottle());

			Rendering.Reconcile();
			TestFalse(TEXT("override back after the save"), BgRenderSpecThrottle());
			TestTrue(TEXT("user's value still kept"), Rendering.GetUserThrottleSetting());
			Rendering.SetHeld(false);
		});

		It("the user turning the throttle off while held is kept on release", [this]()
		{
			if (!GEditor)
				return;
			BgRenderSpecSetThrottle(true);
			FUnrealMcpBackgroundRendering Rendering;
			Rendering.SetHeld(true);

			BgRenderSpecSetThrottle(false); // the details panel writes the new value before PostEditChange
			FPropertyChangedEvent Event = BgRenderSpecEvent(TEXT("bThrottleCPUWhenNotForeground"));
			Rendering.HandleSettingChanged(BgRenderSpecSettings(), Event);
			TestFalse(TEXT("the user's new value is adopted"), Rendering.GetUserThrottleSetting());

			Rendering.SetHeld(false);
			TestFalse(TEXT("released to the user's new value"), BgRenderSpecThrottle());
		});

		It("the user turning the throttle on while held is kept, and the override returns", [this]()
		{
			if (!GEditor)
				return;
			BgRenderSpecSetThrottle(false);
			FUnrealMcpBackgroundRendering Rendering;
			Rendering.SetHeld(true);

			BgRenderSpecSetThrottle(true);
			FPropertyChangedEvent Event = BgRenderSpecEvent(TEXT("bThrottleCPUWhenNotForeground"));
			Rendering.HandleSettingChanged(BgRenderSpecSettings(), Event);
			TestTrue(TEXT("saved as the user set it"), BgRenderSpecThrottle());
			Rendering.Reconcile();
			TestFalse(TEXT("override back"), BgRenderSpecThrottle());

			Rendering.SetHeld(false);
			TestTrue(TEXT("released to the user's new value"), BgRenderSpecThrottle());
		});

		It("is wired to the settings object's OnSettingChanged (a real PostEditChange)", [this]()
		{
			if (!GEditor)
				return;
			BgRenderSpecSetThrottle(true);
			FUnrealMcpBackgroundRendering Rendering;
			Rendering.SetHeld(true);
			BgRenderSpecSettings()->PostEditChange(); // what code that changes and saves the settings calls first
			TestTrue(TEXT("the handler ran: user's value in memory for the save"), BgRenderSpecThrottle());
			Rendering.SetHeld(false);
			BgRenderSpecSettings()->PostEditChange();
			TestTrue(TEXT("no handler after release"), BgRenderSpecThrottle());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
