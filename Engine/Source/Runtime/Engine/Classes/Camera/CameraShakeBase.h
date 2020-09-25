// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/EnumClassFlags.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Object.h"
#include "UObject/ScriptMacros.h"
#include "Camera/CameraTypes.h"
#include "CameraShakeBase.generated.h"

class APlayerCameraManager;
class UCameraShakePattern;

/**
 * Parameters for starting a camera shake.
 */
USTRUCT(BlueprintType)
struct ENGINE_API FCameraShakeStartParams
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=CameraShake)
	bool bIsRestarting = false;
};

/**
 * Parameters for updating a camera shake.
 */
USTRUCT(BlueprintType)
struct ENGINE_API FCameraShakeUpdateParams
{
	GENERATED_BODY()

	FCameraShakeUpdateParams()
	{}

	FCameraShakeUpdateParams(const FMinimalViewInfo& InPOV)
		: POV(InPOV)
	{}

	/** The time elapsed since last update */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=CameraShake)
	float DeltaTime = 0.f;
	/** The dynamic scale being passed down from the camera manger for this shake */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=CameraShake)
	float DynamicScale = 1.f;
	/** The auto-computed blend in/out scale, when blending is handled by base class (see UCameraShakeBase::GetShakeInfo) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=CameraShake)
	float BlendingWeight = 1.f;
	/** The total scale to apply to the camera shake during the current update. Equals ShakeScale * DynamicScale * BlendingWeight */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=CameraShake)
	float TotalScale = 1.f;
	/** The current view that this camera shake should modify */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=CameraShake)
	FMinimalViewInfo POV;
};

/**
 * Flags that camera shakes can return to change base-class behaviour.
 */
UENUM()
enum class ECameraShakeUpdateResultFlags : uint8
{
	/** Apply the result location, rotation, and field of view as absolute values, instead of additive values. */
	ApplyAsAbsolute = 1 << 0,
	/** Do not apply scaling (dynamic scale, blending weight, shake scale), meaning that this will be done in the sub-class. Implied when ApplyAsAbsolute is set. */
	SkipAutoScale = 1 << 1,
	/** Do not re-orient the result based on the play-space. Implied when ApplyAsAbsolute is set. */
	SkipAutoPlaySpace = 1 << 2,

	/** Default flags: the sub-class is returning local, additive offsets, and lets the base class take care of the rest. */
	Default = 0
};
ENUM_CLASS_FLAGS(ECameraShakeUpdateResultFlags);

/**
 * The result of a camera shake update.
 */
USTRUCT(BlueprintType)
struct ENGINE_API FCameraShakeUpdateResult
{
	GENERATED_BODY()

	FCameraShakeUpdateResult()
		: Location(FVector::ZeroVector)
		, Rotation(FRotator::ZeroRotator)
		, FOV(0.f)
		, Flags(ECameraShakeUpdateResultFlags::Default)
		, bIsFinished(false)
	{}

	/** Location offset for the view, or new absolute location if ApplyAsAbsolute flag is set */
	FVector Location;
	/** Rotation offset for the view, or new absolute rotation if ApplyAsAbsolute flag is set */
	FRotator Rotation;
	/** Field-of-view offset for the view, or new absolute field-of-view if ApplyAsAbsolute flag is set */
	float FOV;

	/** Flags for how the base class should handle the result */
	ECameraShakeUpdateResultFlags Flags;

	/** Whether the camera shake is finished, for when duration isn't handled by the base class (see UCameraShakeBase::GetShakeInfo) */
	bool bIsFinished;
};

/**
 * Parameters for stopping a camera shake.
 */
USTRUCT(BlueprintType)
struct ENGINE_API FCameraShakeStopParams
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=CameraShake)
	bool bImmediately = false;
};

/**
 * Camera shake duration type.
 */
UENUM()
enum class ECameraShakeDurationType : uint8
{
	/** Camera shake has a fixed duration */
	Fixed,
	/** Camera shake is playing indefinitely, until explicitly stopped */
	Infinite,
	/** Camera shake has custom/dynamic duration */
	Custom
};

/**
 * Camera shake duration.
 */
USTRUCT(BlueprintType)
struct ENGINE_API FCameraShakeDuration
{
	GENERATED_BODY()

	/** Returns an infinite shake duration */
	static FCameraShakeDuration Infinite() { return FCameraShakeDuration { 0.f, ECameraShakeDurationType::Infinite }; }
	/** Returns a custom shake duration */
	static FCameraShakeDuration Custom() { return FCameraShakeDuration { 0.f, ECameraShakeDurationType::Custom }; }

	/** Creates a new shake duration */
	FCameraShakeDuration() : Duration(0.f), Type(ECameraShakeDurationType::Fixed) {}
	/** Creates a new shake duration */
	FCameraShakeDuration(float InDuration, ECameraShakeDurationType InType = ECameraShakeDurationType::Fixed) : Duration(InDuration), Type(InType) {}
	
	/** Returns whether this duration is a fixed time */
	bool IsFixed() const { return Type == ECameraShakeDurationType::Fixed; }
	/** Returns whether this duration is infinite */
	bool IsInfinite() const { return Type == ECameraShakeDurationType::Infinite; }
	/** Returns whether this duration is custom */
	bool IsCustom() const { return Type == ECameraShakeDurationType::Custom; }

	/** When the duration is fixed, return the duration time */
	float Get() const { check(Type == ECameraShakeDurationType::Fixed); return Duration; }

private:
	UPROPERTY()
	float Duration;

	UPROPERTY()
	ECameraShakeDurationType Type;
};

/**
 * Information about a camera shake class.
 */
USTRUCT(BlueprintType)
struct ENGINE_API FCameraShakeInfo
{
	GENERATED_BODY()

	/** The duration of the camera shake */
	UPROPERTY()
	FCameraShakeDuration Duration;

	/** How much blending-in the camera shake should have */
	UPROPERTY()
	float BlendIn = 0.f;

	/** How much blending-out the camera shake should have */
	UPROPERTY()
	float BlendOut = 0.f;
};

/**
 * Base class for a camera shake. A camera shake contains a root shake "pattern" which is
 * the object that contains the actual logic driving how the camera is shaken. Keeping the two
 * separate makes it possible to completely change how a shake works without having to create
 * a completely different asset.
 *
 * Note that this class is marked as "abstract" so that UCameraShakeInstance (defined in the
 * GameplayCameras plug-in) is used as the default type, which adds a perlin noise pattern
 * as its root pattern.
 */
UCLASS(Abstract, Blueprintable, EditInlineNew)
class ENGINE_API UCameraShakeBase : public UObject
{
	GENERATED_BODY()

public:

	/** Create a new instance of a camera shake */
	UCameraShakeBase(const FObjectInitializer& ObjectInitializer);
	
public:

	/**
	 * Gets the duration of this camera shake in seconds.
	 *
	 * The value could be 0 or negative if the shake uses the oscillator, meaning, respectively,
	 * no oscillation, or indefinite oscillation.
	 */
	FCameraShakeDuration GetCameraShakeDuration() const;

	/**
	 * Gets the duration of this camera shake's blend in and out.
	 *
	 * The values could be 0 or negative if there's no blend in and/or out.
	 */
	void GetCameraShakeBlendTimes(float& OutBlendIn, float& OutBlendOut) const;

	/**
	 * Gets the default duration for camera shakes of the given class.
	 *
	 * @param CameraShakeClass    The class of camera shake
	 * @param OutDuration         Will store the default duration of the given camera shake class, if possible
	 * @return                    Whether a valid default duration was found
	 */
	static bool GetCameraShakeDuration(TSubclassOf<UCameraShakeBase> CameraShakeClass, FCameraShakeDuration& OutDuration)
	{
		if (CameraShakeClass)
		{
			if (const UCameraShakeBase* CDO = CameraShakeClass->GetDefaultObject<UCameraShakeBase>())
			{
				OutDuration = CDO->GetCameraShakeDuration();
				return true;
			}
		}
		return false;
	}

	/**
	 * Gets the default blend in/out durations for camera shakes of the given class.
	 *
	 * @param CameraShakeClass    The class of camera shake
	 * @param OutBlendIn          Will store the default blend-in time of the given camera shake class, if possible
	 * @param OutBlendOut         Will store the default blend-out time of the given camera shake class, if possible
	 * @return                    Whether valid default blend in/out times were found
	 */
	static bool GetCameraShakeBlendTimes(TSubclassOf<UCameraShakeBase> CameraShakeClass, float& OutBlendIn, float& OutBlendOut)
	{
		if (CameraShakeClass)
		{
			if (const UCameraShakeBase* CDO = CameraShakeClass->GetDefaultObject<UCameraShakeBase>())
			{
				CDO->GetCameraShakeBlendTimes(OutBlendIn, OutBlendOut);
				return true;
			}
		}
		return false;
	}

public:
	/** 
	 *  If true to only allow a single instance of this shake class to play at any given time.
	 *  Subsequent attempts to play this shake will simply restart the timer.
	 */
	UPROPERTY(EditAnywhere, Category=CameraShake)
	bool bSingleInstance;

	/** The overall scale to apply to the shake. Only valid when the shake is active. */
	UPROPERTY(transient, BlueprintReadWrite, Category=CameraShake)
	float ShakeScale;

	/** Gets the root pattern of this camera shake */
	UFUNCTION(BlueprintPure, Category="CameraShake")
	UCameraShakePattern* GetRootShakePattern() const { return RootShakePattern; }

	/** Sets the root pattern of this camera shake */
	UFUNCTION(BlueprintCallable, Category="CameraShake")
	void SetRootShakePattern(UCameraShakePattern* InPattern);

	/** Creates a new pattern of the given type and sets it as the root one on this shake */
	template<typename ShakePatternType>
	ShakePatternType* ChangeRootShakePattern()
	{
		ShakePatternType* ShakePattern = NewObject<ShakePatternType>(this);
		SetRootShakePattern(ShakePattern);
		return ShakePattern;
	}

public:

	/** Gets some infromation about this specific camera shake */
	void GetShakeInfo(FCameraShakeInfo& OutInfo) const;

	/** Starts this camera shake with the given parameters */
	void StartShake(APlayerCameraManager* Camera, float Scale, ECameraShakePlaySpace InPlaySpace, FRotator UserPlaySpaceRot = FRotator::ZeroRotator);

	/** Returns whether this camera shake is finished */
	bool IsFinished() const;

	/** Updates this camera shake and applies its effect to the given view */
	void UpdateAndApplyCameraShake(float DeltaTime, float Alpha, FMinimalViewInfo& InOutPOV);

	/** Stops this camera shake */
	void StopShake(bool bImmediately = true);

	/** Tears down this camera shake before destruction or recycling */
	void TeardownShake();

public:

	/** Gets the current camera manager. Will be null if the shake isn't active. */
	APlayerCameraManager* GetCameraManager() const { return CameraManager; }

	/** Returns the current play space. The value is irrelevant if the shake isn't active. */
	ECameraShakePlaySpace GetPlaySpace() const { return PlaySpace; }
	/** Returns the current play space matrix. The value is irrelevant if the shake isn't active, or if its play space isn't UserDefined. */
	const FMatrix& GetUserPlaySpaceMatrix() const { return UserPlaySpaceMatrix; }
	/** Sets the current play space matrix. This method has no effect if the shake isn't active, or if its play space isn't UserDefined. */
	void SetUserPlaySpaceMatrix(const FMatrix& InMatrix) { UserPlaySpaceMatrix = InMatrix; }

protected:

	/** Applies all the appropriate auto-scaling to the current shake offset (only if the result is "relative") */
	void ApplyScale(const FCameraShakeUpdateParams& Params, FCameraShakeUpdateResult& InOutResult) const;

	/** Applies the given scale to the current shake offset (only if the result is "relative") */
	void ApplyScale(float Scale, FCameraShakeUpdateResult& InOutResult) const;

	/** Applies any appropriate system-wide limits */
	void ApplyLimits(const FCameraShakeUpdateParams& Params, FCameraShakeUpdateResult& InOutResult) const;

	/**
	 * Modifies the current shake offset to be oriented in the current shake's play space (only if the result is "relative")
	 *
	 * Note that this modifies the result and makes it "absolute".
	 */
	void ApplyPlaySpace(const FCameraShakeUpdateParams& Params, FCameraShakeUpdateResult& InOutResult) const;

private:

	/** The root pattern for this camera shake */
	UPROPERTY(EditAnywhere, Instanced, Category=CameraShake)
	UCameraShakePattern* RootShakePattern;

	/** The camera manager owning this camera shake. Only valid when the shake is active. */
	UPROPERTY(transient)
	APlayerCameraManager* CameraManager;

	/** What space to play the shake in before applying to the camera. Only valid when the shake is active. */
	ECameraShakePlaySpace PlaySpace;

	/** Matrix defining a custom play space, used when PlaySpace is UserDefined. Only valid when the shake is active. */
	FMatrix UserPlaySpaceMatrix;

	/** Information about our shake's specific implementation. Only valid when the shake is active. */
	FCameraShakeInfo ActiveInfo;

	/** Transitive state of the shake. Only valid when the shake is active. */
	struct FCameraShakeState
	{
		FCameraShakeState() : 
			ElapsedTime(0.f)
			, bIsActive(false)
			, bHasDuration(false)
			, bHasBlendIn(false)
			, bHasBlendOut(false)
		{}

		float ElapsedTime;
		bool bIsActive : 1;
		bool bHasDuration : 1;
		bool bHasBlendIn : 1;
		bool bHasBlendOut : 1;
	};
	FCameraShakeState State;
};

/**
 * A shake "pattern" defines how a camera should be effectively shaken. Examples of shake patterns
 * are sinewave oscillation, perlin noise, or FBX animation.
 *
 */
UCLASS(Abstract, EditInlineNew)
class ENGINE_API UCameraShakePattern : public UObject
{
	GENERATED_BODY()

public:

	/** Constructor for a shake pattern */
	UCameraShakePattern(const FObjectInitializer& ObjectInitializer);

	/** Gets information about this shake pattern */
	void GetShakePatternInfo(FCameraShakeInfo& OutInfo) const;
	/** Called when the shake pattern starts */
	void StartShakePattern(const FCameraShakeStartParams& Params);
	/** Updates the shake pattern, which should add its generated offset to the given result */
	void UpdateShakePattern(const FCameraShakeUpdateParams& Params, FCameraShakeUpdateResult& OutResult);
	/** Returns whether this shake pattern is finished */
	bool IsFinished() const;
	/** Called when the shake pattern is manually stopped */
	void StopShakePattern(const FCameraShakeStopParams& Params);
	/** Call when the shake pattern is discard, either after naturally finishing or being stopped manually */
	void TeardownShakePattern();

protected:

	/** Gets the shake pattern's parent shake */
	UCameraShakeBase* GetShakeInstance() const;

	/** Gets the shake pattern's parent shake */
	template<typename InstanceType>
	InstanceType* GetShakeInstance() const { return Cast<InstanceType>(GetShakeInstance()); }

private:

	// UCameraShakePattern interface
	virtual void GetShakePatternInfoImpl(FCameraShakeInfo& OutInfo) const {}
	virtual void StartShakePatternImpl(const FCameraShakeStartParams& Params) {}
	virtual void UpdateShakePatternImpl(const FCameraShakeUpdateParams& Params, FCameraShakeUpdateResult& OutResult) {}
	virtual bool IsFinishedImpl() const { return true; }
	virtual void StopShakePatternImpl(const FCameraShakeStopParams& Params) {}
	virtual void TeardownShakePatternImpl()  {}
};

