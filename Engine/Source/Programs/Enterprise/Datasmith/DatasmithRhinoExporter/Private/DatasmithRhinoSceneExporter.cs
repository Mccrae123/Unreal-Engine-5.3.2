// Copyright Epic Games, Inc. All Rights Reserved.

using Rhino;
using Rhino.DocObjects;
using Rhino.Geometry;
using System;
using System.Collections.Generic;
using System.Collections.Specialized;

namespace DatasmithRhino
{
	public static class DatasmithRhinoSceneExporter
	{
		public static bool Export(string Filename, RhinoDoc RhinoDocument, Rhino.FileIO.FileWriteOptions Options)
		{
			string RhinoAppName = Rhino.RhinoApp.Name;
			string RhinoVersion = Rhino.RhinoApp.ExeVersion.ToString();
			FDatasmithFacadeElement.SetCoordinateSystemType(FDatasmithFacadeElement.ECoordinateSystemType.RightHandedZup);
			FDatasmithFacadeElement.SetWorldUnitScale((float)Rhino.RhinoMath.UnitScale(RhinoDocument.ModelUnitSystem, UnitSystem.Centimeters));
			FDatasmithFacadeScene DatasmithScene = new FDatasmithFacadeScene("Rhino", "Robert McNeel & Associates", "Rhino3D", RhinoVersion);
			DatasmithScene.PreExport();

			try
			{
				FDatasmithRhinoProgressManager.Instance.StartMainTaskProgress("Parsing Document", 0.1f);
				DatasmithRhinoSceneParser SceneParser = new DatasmithRhinoSceneParser(RhinoDocument, Options);
				SceneParser.ParseDocument();

				if (ExportScene(SceneParser, DatasmithScene) == Rhino.Commands.Result.Success)
				{
					string SceneName = System.IO.Path.GetFileName(Filename);

					FDatasmithRhinoProgressManager.Instance.StartMainTaskProgress("Writing to files..", 1);
					DatasmithScene.ExportScene(Filename);
				}
			}
			catch (Exception)
			{
				return false;
			}
			finally
			{
				FDatasmithRhinoProgressManager.Instance.StopProgress();
			}

			return true;
		}

		public static Rhino.Commands.Result ExportScene(DatasmithRhinoSceneParser SceneParser, FDatasmithFacadeScene DatasmithScene)
		{
			FDatasmithRhinoProgressManager.Instance.StartMainTaskProgress("Exporting Materials", 0.2f);
			FDatasmithRhinoMaterialExporter.ExportMaterials(DatasmithScene, SceneParser);

			FDatasmithRhinoProgressManager.Instance.StartMainTaskProgress("Exporting Meshes", 0.7f);
			FDatasmithRhinoMeshExporter.ExportMeshes(DatasmithScene, SceneParser);

			FDatasmithRhinoProgressManager.Instance.StartMainTaskProgress("Exporting Actors", 0.8f);
			ExportActors(DatasmithScene, SceneParser);

			return Rhino.Commands.Result.Success;
		}

		private static void ExportActors(FDatasmithFacadeScene DatasmithScene, DatasmithRhinoSceneParser SceneParser)
		{
			foreach (RhinoSceneHierarchyNode Node in SceneParser.SceneRoot)
			{
				if (Node.bIsRoot)
				{
					continue;
				}

				ExportHierarchyNode(DatasmithScene, Node, SceneParser);
			}
		}

		private static void ExportHierarchyNode(FDatasmithFacadeScene DatasmithScene, RhinoSceneHierarchyNode Node, DatasmithRhinoSceneParser SceneParser)
		{
			if (Node.Info.bHasRhinoObject)
			{
				ExportObject(DatasmithScene, Node, SceneParser);
			}
			else
			{
				//This node has no RhinoObject, export an empty Actor.
				ExportEmptyNode(DatasmithScene, Node);
			}
		}

		private static void ExportObject(FDatasmithFacadeScene InDatasmithScene, RhinoSceneHierarchyNode InNode, DatasmithRhinoSceneParser SceneParser)
		{
			RhinoObject CurrentObject = InNode.Info.RhinoModelComponent as RhinoObject;

			if (CurrentObject.ObjectType == ObjectType.InstanceReference
				|| CurrentObject.ObjectType == ObjectType.Point)
			{
				//The Instance Reference node is exported as an empty actor under which we create the instanced block.
				//Export points as empty actors as well.
				ExportEmptyNode(InDatasmithScene, InNode);
			}
			else if (CurrentObject.ObjectType == ObjectType.Light)
			{
				ExportLightObject(InDatasmithScene, InNode);
			}
			else if (SceneParser.ObjectIdToMeshInfoDictionary.TryGetValue(CurrentObject.Id, out DatasmithMeshInfo MeshInfo))
			{
				string HashedActorName = FDatasmithFacadeActor.GetStringHash("A:" + InNode.Info.Name);
				FDatasmithFacadeActorMesh DatasmithActorMesh = new FDatasmithFacadeActorMesh(HashedActorName);
				DatasmithActorMesh.SetLabel(InNode.Info.Label);
				InNode.SetDatasmithActor(DatasmithActorMesh);

				AddTagsToDatasmithActor(DatasmithActorMesh, InNode);
				AddMetadataToDatasmithActor(DatasmithActorMesh, InNode, InDatasmithScene);
				AddLayersToDatasmithActor(DatasmithActorMesh, InNode);

				Transform OffsetTransform = Transform.Translation(MeshInfo.PivotOffset);
				Transform WorldTransform = Transform.Multiply(InNode.Info.WorldTransform, OffsetTransform);
				DatasmithActorMesh.SetWorldTransform(WorldTransform.ToFloatArray(false));

				string MeshName = FDatasmithFacadeElement.GetStringHash(MeshInfo.Name);
				DatasmithActorMesh.SetMesh(MeshName);

				if (InNode.Info.bOverrideMaterial)
				{
					RhinoMaterialInfo MaterialInfo = SceneParser.GetMaterialInfoFromMaterialIndex(InNode.Info.MaterialIndex);
					DatasmithActorMesh.AddMaterialOverride(MaterialInfo.Name, 0);
				}

				AddActorToParent(DatasmithActorMesh, InNode, InDatasmithScene);
			}
			else
			{
				//TODO Log non-exported object in DatasmithExport UI (Writing to Rhino Console is extremely slow).
			}
		}

		private static void ExportEmptyNode(FDatasmithFacadeScene InDatasmithScene, RhinoSceneHierarchyNode InNode)
		{
			string HashedName = FDatasmithFacadeElement.GetStringHash(InNode.Info.Name);
			FDatasmithFacadeActor DatasmithActor = new FDatasmithFacadeActor(HashedName);
			DatasmithActor.SetLabel(InNode.Info.Label);
			InNode.SetDatasmithActor(DatasmithActor);
			AddTagsToDatasmithActor(DatasmithActor, InNode);
			AddMetadataToDatasmithActor(DatasmithActor, InNode, InDatasmithScene);
			AddLayersToDatasmithActor(DatasmithActor, InNode);

			float[] MatrixArray = InNode.Info.WorldTransform.ToFloatArray(false);
			DatasmithActor.SetWorldTransform(MatrixArray);

			AddActorToParent(DatasmithActor, InNode, InDatasmithScene);
		}

		private static void ExportLightObject(FDatasmithFacadeScene InDatasmithScene, RhinoSceneHierarchyNode InNode)
		{
			LightObject RhinoLightObject = InNode.Info.RhinoModelComponent as LightObject;

			FDatasmithFacadeActorLight DatasmithActorLight = SetupLightActor(InNode.Info, RhinoLightObject.LightGeometry);
			if (DatasmithActorLight != null)
			{
				InNode.SetDatasmithActor(DatasmithActorLight);
				AddTagsToDatasmithActor(DatasmithActorLight, InNode);
				AddMetadataToDatasmithActor(DatasmithActorLight, InNode, InDatasmithScene);
				AddLayersToDatasmithActor(DatasmithActorLight, InNode);

				float[] MatrixArray = InNode.Info.WorldTransform.ToFloatArray(false);
				DatasmithActorLight.SetWorldTransform(MatrixArray);

				AddActorToParent(DatasmithActorLight, InNode, InDatasmithScene);
			}
			else
			{
				// #ueent_todo: Log non supported light type.
				ExportEmptyNode(InDatasmithScene, InNode);
			}
		}

		private static void AddActorToParent(FDatasmithFacadeActor InDatasmithActor, RhinoSceneHierarchyNode InNode, FDatasmithFacadeScene InDatasmithScene)
		{
			if (InNode.Parent.bIsRoot)
			{
				InDatasmithScene.AddActor(InDatasmithActor);
			}
			else
			{
				InNode.Parent.DatasmithActor.AddChild(InDatasmithActor);
			}
		}

		private static void AddTagsToDatasmithActor(FDatasmithFacadeActor InDatasmithActor, RhinoSceneHierarchyNode InNode)
		{
			if (!InNode.bIsRoot && InNode.Info.Tags != null)
			{
				foreach (string CurrentTag in InNode.Info.Tags)
				{
					InDatasmithActor.AddTag(CurrentTag);
				}
			}
		}

		private static void AddMetadataToDatasmithActor(FDatasmithFacadeActor InDatasmithActor, RhinoSceneHierarchyNode InNode, FDatasmithFacadeScene InDatasmithScene)
		{
			if (!InNode.Info.bHasRhinoObject)
			{
				return;
			}

			RhinoObject NodeObject = InNode.Info.RhinoModelComponent as RhinoObject;
			NameValueCollection UserStrings = NodeObject.Attributes.GetUserStrings();

			if (UserStrings != null && UserStrings.Count > 0)
			{
				string[] Keys = UserStrings.AllKeys;
				FDatasmithFacadeMetaData DatasmithMetaData = new FDatasmithFacadeMetaData(InDatasmithActor.GetName() + "_DATA");
				DatasmithMetaData.SetLabel(InDatasmithActor.GetLabel());
				DatasmithMetaData.SetAssociatedElement(InDatasmithActor);

				for (int KeyIndex = 0; KeyIndex < Keys.Length; ++KeyIndex)
				{
					string CurrentKey = Keys[KeyIndex];
					string EvaluatedValue = FDatasmithRhinoUtilities.EvaluateAttributeUserText(InNode, UserStrings.Get(CurrentKey));

					DatasmithMetaData.AddPropertyString(CurrentKey, EvaluatedValue);
				}

				InDatasmithScene.AddMetaData(DatasmithMetaData);
			}
		}

		private static void AddLayersToDatasmithActor(FDatasmithFacadeActor InDatasmithActor, RhinoSceneHierarchyNode InNode)
		{
			string Layer = "";
			if (InNode.Parent != null && InNode.Parent.DatasmithActor != null)
			{
				string ParentLayer = InNode.Parent.DatasmithActor.GetLayer();
				if (!string.IsNullOrEmpty(ParentLayer))
				{
					Layer = ParentLayer;
				}
			}

			if (InNode.Info.bHasRhinoLayer)
			{
				if (string.IsNullOrEmpty(Layer))
				{
					Layer = InNode.Info.Label;
				}
				else
				{
					Layer = string.Format("{0}_{1}", Layer, InNode.Info.Label);
				}
			}

			InDatasmithActor.SetLayer(Layer);
		}

		private static FDatasmithFacadeActorLight SetupLightActor(RhinoSceneHierarchyNodeInfo HierarchyNodeInfo, Light RhinoLight)
		{
			LightObject RhinoLightObject = HierarchyNodeInfo.RhinoModelComponent as LightObject;
			string HashedName = FDatasmithFacadeElement.GetStringHash(HierarchyNodeInfo.Name);
			FDatasmithFacadeActorLight LightElement;

			switch (RhinoLight.LightStyle)
			{
				case LightStyle.CameraSpot:
				case LightStyle.WorldSpot:
					FDatasmithFacadeSpotLight SpotLightElement = new FDatasmithFacadeSpotLight(HashedName);
					LightElement = SpotLightElement;
					double OuterSpotAngle = FDatasmithRhinoUtilities.RadianToDegree(RhinoLight.SpotAngleRadians);
					double InnerSpotAngle = RhinoLight.HotSpot * OuterSpotAngle;

					SpotLightElement.SetOuterConeAngle((float)OuterSpotAngle);
					SpotLightElement.SetInnerConeAngle((float)InnerSpotAngle);
					break;

				case LightStyle.WorldLinear:
				case LightStyle.WorldRectangular:
					FDatasmithFacadeAreaLight AreaLightElement = new FDatasmithFacadeAreaLight(HashedName);
					LightElement = AreaLightElement;
					double Length = RhinoLight.Length.Length;
					AreaLightElement.SetLength((float)Length);

					if (RhinoLight.IsRectangularLight)
					{
						double Width = RhinoLight.Width.Length;

						AreaLightElement.SetWidth((float)Width);
						AreaLightElement.SetLightShape(FDatasmithFacadeAreaLight.EAreaLightShape.Rectangle);
						AreaLightElement.SetLightType(FDatasmithFacadeAreaLight.EAreaLightType.Rect);
					}
					else
					{
						AreaLightElement.SetWidth((float)(0.01f * Length));
						AreaLightElement.SetLightShape(FDatasmithFacadeAreaLight.EAreaLightShape.Cylinder);
						AreaLightElement.SetLightType(FDatasmithFacadeAreaLight.EAreaLightType.Point);
						// The light in Rhino doesn't have attenuation, but the attenuation radius was found by testing in Unreal to obtain a visual similar to Rhino
						float DocumentScale = (float)Rhino.RhinoMath.UnitScale(Rhino.RhinoDoc.ActiveDoc.ModelUnitSystem, UnitSystem.Centimeters);
						AreaLightElement.SetAttenuationRadius(1800f / DocumentScale);
					}
					break;
				case LightStyle.CameraDirectional:
				case LightStyle.WorldDirectional:
					LightElement = new FDatasmithFacadeDirectionalLight(HashedName);

					break;
				case LightStyle.CameraPoint:
				case LightStyle.WorldPoint:
					LightElement = new FDatasmithFacadePointLight(HashedName);
					break;
				case LightStyle.Ambient: // not supported as light
				default:
					LightElement = null;
					break;
			}

			if(LightElement != null)
			{
				System.Drawing.Color DiffuseColor = RhinoLight.Diffuse;
				LightElement.SetColor(DiffuseColor.R, DiffuseColor.G, DiffuseColor.B, DiffuseColor.A);
				LightElement.SetIntensity(RhinoLight.Intensity * 100f);
				LightElement.SetEnabled(RhinoLight.IsEnabled);
				LightElement.SetLabel(HierarchyNodeInfo.Label);

				FDatasmithFacadePointLight PointLightElement = LightElement as FDatasmithFacadePointLight;
				if (PointLightElement != null)
				{
					PointLightElement.SetIntensityUnits(FDatasmithFacadePointLight.EPointLightIntensityUnit.Candelas);
				}
			}

			return LightElement;
		}
	}
}