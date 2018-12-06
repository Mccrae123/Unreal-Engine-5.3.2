#include "AxesParamAsset.h"
#include "3dRudderDevice.h"

DEFINE_LOG_CATEGORY_STATIC(_3dRudderAsset, Log, All)
#define LOCTEXT_NAMESPACE "3dRudderAsset"

FMyCurve::FMyCurve()
{
	DeadZone = 0.0f;
	Sensitivity = 1.0f;
	Shape = 1.0f;
}

FMyCurve::FMyCurve(float deadzone, float sensitivity, float shape)
{
	DeadZone = deadzone;
	Sensitivity = sensitivity;
	Shape = shape;
}

UAxesParamAsset::UAxesParamAsset(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	AxesParamType = E3dRudderAxesParam::Custom;
	NonSymmetricalPitch = true;
	RollToYawCompensation = 0.15f;
	LeftRight = FMyCurve(0.15f, 1.0f, 2.0f);
	ForwardBackward = FMyCurve(0.15f, 1.0f, 2.0f);
	UpDown = FMyCurve(0.08f, 0.6f, 4.0f);
	Rotation = FMyCurve(0.15f, 1.0f, 2.0f);
	Test = false;
	PortNumber = 0;	
	CreateAxesParam();
}

#if WITH_EDITOR
void UAxesParamAsset::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName Name = (PropertyChangedEvent.Property != nullptr) ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	//UE_LOG(_3DRudderAsset, Warning, TEXT("property change: %s"), *PropertyChangedEvent.Property->GetNameCPP());
	if (Name.ToString() == "AxesParamType")
	{
		CreateAxesParam();
	}
	else if (Name.ToString() == "NonSymmetricalPitch")
	{
		pAxesParam->SetNonSymmetrical(NonSymmetricalPitch);
	}
	else if (Name.ToString() == "RollToYawCompensation")
	{
		pAxesParam->SetRoll2YawCompensation(RollToYawCompensation);
	}
}
#endif

void UAxesParamAsset::PostLoad()
{
	Super::PostLoad();
	//UE_LOG(_3DRudderAsset, Warning, TEXT("post load %d"), (int)AxesParamType);
	CreateAxesParam();
}

void UAxesParamAsset::CreateAxesParam()
{
	switch (AxesParamType)
	{
		case E3dRudderAxesParam::NormalizedLinear:
		{
			pAxesParam = new ns3dRudder::AxesParamNormalizedLinear();
			NonSymmetricalPitch = false;
			RollToYawCompensation = 0.0f;
			break;
		}
		case E3dRudderAxesParam::Custom:
		{
			pAxesParam = new CAxesParamCustom(this);
			break;
		}
		default:
		{
			pAxesParam = new ns3dRudder::AxesParamDefault();
			NonSymmetricalPitch = true;
			RollToYawCompensation = 0.15f;
			break;
		}
	}
}

void UAxesParamAsset::BeginDestroy()
{
	if (pAxesParam != nullptr)
	{
		delete pAxesParam;
		pAxesParam = nullptr;
	}
	Super::BeginDestroy();	
}

CAxesParamCustom::CAxesParamCustom(UAxesParamAsset* passet )
{
	asset = passet;
	SetCurve(ns3dRudder::LeftRight, &m_Curve[ns3dRudder::LeftRight]);
	SetCurve(ns3dRudder::ForwardBackward, &m_Curve[ns3dRudder::ForwardBackward]);
	SetCurve(ns3dRudder::Rotation, &m_Curve[ns3dRudder::Rotation]);
	SetCurve(ns3dRudder::UpDown, &m_Curve[ns3dRudder::UpDown]);

}

ns3dRudder::ErrorCode CAxesParamCustom::UpdateParam(uint32_t nPortNumber)
{
	//UE_LOG(_3DRudderAsset, Warning, TEXT("update"));
	ns3dRudder::ErrorCode nError = ns3dRudder::Success;

	SetNonSymmetrical(asset->NonSymmetricalPitch);
	SetRoll2YawCompensation(asset->RollToYawCompensation);

	float ratioPitch = 1.0f;
	float ratioRoll = 1.0f;
	float ratioYaw = 1.0f;

	ns3dRudder::DeviceInformation *pInfo = F3dRudderDevice::s_pSdk->GetDeviceInformation(nPortNumber);
	if (pInfo != nullptr)
	{
		ratioRoll = pInfo->GetUserRoll() / pInfo->GetMaxRoll();
		ratioPitch = pInfo->GetUserPitch() / pInfo->GetMaxPitch();
		ratioYaw = pInfo->GetUserYaw() / pInfo->GetMaxYaw();
	}

	if (asset != nullptr)
	{
		float fxSat = asset->LeftRight.Sensitivity * ratioRoll;
		ns3dRudder::Curve* pCurve = GetCurve(ns3dRudder::LeftRight);
		pCurve->SetDeadZone(asset->LeftRight.DeadZone * fxSat);
		pCurve->SetXSat(fxSat);
		pCurve->SetExp(asset->LeftRight.Shape);
		
		fxSat = asset->ForwardBackward.Sensitivity * ratioPitch;
		pCurve = GetCurve(ns3dRudder::ForwardBackward);
		pCurve->SetDeadZone(asset->ForwardBackward.DeadZone * fxSat);
		pCurve->SetXSat(fxSat);
		pCurve->SetExp(asset->ForwardBackward.Shape);

		fxSat = asset->Rotation.Sensitivity * ratioYaw;
		pCurve = GetCurve(ns3dRudder::Rotation);
		pCurve->SetDeadZone(asset->Rotation.DeadZone * fxSat);
		pCurve->SetXSat(fxSat);
		pCurve->SetExp(asset->Rotation.Shape);

		pCurve = GetCurve(ns3dRudder::UpDown);
		pCurve->SetDeadZone(asset->UpDown.DeadZone);
		pCurve->SetXSat(asset->UpDown.Sensitivity);
		pCurve->SetExp(asset->UpDown.Shape);
	}
	else
		nError = F3dRudderDevice::s_pSdk->GetLastError();
	return nError;
}

#undef LOCTEXT_NAMESPACE