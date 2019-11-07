// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#include "ModelingOperators\Public\SpaceDeformerOps\BendMeshOp.h"


//Bends along the Y-axis
void FBendMeshOp::CalculateResult(FProgressCancel* Progress)
{


	// Matrix from gizmo space (z-up) to a y-up space
	FMatrix ToYUp(EForceInit::ForceInitToZero);
	ToYUp.M[0][0] =  1.f;
	ToYUp.M[1][2] =  1.f;
	ToYUp.M[2][1] = -1.f;
	ToYUp.M[3][3] =  1.f; 
	
	// Full transform to y-up in gizmo space
	FMatrix ObjectToYUpGizmo(EForceInit::ForceInitToZero);
	{
		for (int i = 0; i < 4; ++i)
		{
			for (int j = 0; j < 4; ++j)
			{
				for (int k = 0; k < 4; ++k)
				{
					ObjectToYUpGizmo.M[i][j] += ToYUp.M[i][k] * ObjectToGizmo.M[k][j];
				}
			}
		}
	}


	float Det = ObjectToYUpGizmo.Determinant();

	// Check if the transform is nearly singular
	// this could happen if the scale on the object to world transform has a very small component.
	if (FMath::Abs(Det) < 1.e-4)
	{
		return;
	}

	FMatrix GizmoToObject = ObjectToYUpGizmo.Inverse();
	
	
	const double Curvature = GetModifierValue();
	
	// early out if nothing has been requested.
	if (FMath::Abs(Curvature) < 0.001)
	{
		return;
	}



	const double DegreesToRadians = 0.017453292519943295769236907684886; // Pi / 180
	const double ThetaRadians = DegreesToRadians * Curvature;
	//const FVector3d OpOrigin = ToOpSpace * AxisOriginObjectSpace;
	
	
	// bounds in Op space
	const double YMin =  -LowerBoundsInterval * AxesHalfLength;
	const double YMax =   UpperBoundsInterval * AxesHalfLength;

	const double Y0 = 0.;
	const double K = ThetaRadians / AxesHalfLength;
	const double Ik = 1.0 / K;

	

	for (int VertexID : TargetMesh->VertexIndicesItr())
	{
		//const FVector3d& SrcPos = OriginalPositions[VertexID];
		const FVector3d SrcPos = TargetMesh->GetVertex(VertexID);
		const double SrcPos4[4] = { SrcPos[0], SrcPos[1], SrcPos[2], 1.0};

		// Position in gizmo space
		double GizmoPos4[4] = { 0., 0., 0., 0. };
		for (int i = 0; i < 4; ++i)
		{
			for (int j = 0; j < 4; ++j)
			{
				GizmoPos4[i] += ObjectToYUpGizmo.M[i][j] * SrcPos4[j];
			}
		}


		//FVector3d DstPos = ToOpSpace * (SrcPos) - OpOrigin;

		const double YHat = FMath::Clamp(GizmoPos4[1], YMin, YMax);

		const double Theta = K * (YHat - Y0);

		const double S0 = TMathUtil<double>::Sin(Theta);
		const double C0 = TMathUtil<double>::Cos(Theta);

		const double ZP = GizmoPos4[2] - Ik;

		
		double Y = -S0 * ZP + Y0;
		double Z =  C0 * ZP + Ik;

		if (GizmoPos4[1] > YMax)
		{
			const double YDiff = GizmoPos4[1] - YMax;
			Y += C0 * YDiff;
			Z += S0 * YDiff;
		}
		else if (GizmoPos4[1] < YMin)
		{
			const double YDiff = GizmoPos4[1] - YMin;
			Y += C0 * YDiff;
			Z += S0 * YDiff;
		}

		GizmoPos4[1] = Y;
		GizmoPos4[2] = Z;

		// Position in Obj Space
		double DstPos4[4] = { 0., 0., 0., 0. };
		for (int i = 0; i < 4; ++i)
		{
			for (int j = 0; j < 4; ++j)
			{
				DstPos4[i] += GizmoToObject.M[i][j] * GizmoPos4[j];
			}
		}

		TargetMesh->SetVertex(VertexID, FVector3d(DstPos4[0], DstPos4[1], DstPos4[2]));
	}

}