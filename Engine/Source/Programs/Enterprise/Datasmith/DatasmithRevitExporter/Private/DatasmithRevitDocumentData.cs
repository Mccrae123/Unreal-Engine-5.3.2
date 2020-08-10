// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography.X509Certificates;
using Autodesk.Revit.DB;
using Autodesk.Revit.DB.Architecture;
using Autodesk.Revit.DB.Mechanical;
using Autodesk.Revit.DB.Plumbing;
using Autodesk.Revit.DB.Structure;
using Autodesk.Revit.DB.Visual;

namespace DatasmithRevitExporter
{
	public class FDocumentData
	{
		public enum EActorModifiedFlags
		{
			ActorModifiedProperties = 1 << 0,
			ActorModifiedMetadata = 1 << 1,
			ActorModifiedAll = ~0
		}

		public class FBaseElementData
		{
			public ElementType BaseElementType;
			public FDatasmithFacadeMesh ElementMesh = null;
			public FDatasmithFacadeActor ElementActor = null;
			public FDatasmithFacadeMetaData ElementMetaData = null;
			public EActorModifiedFlags ModifiedFlags = 0;
			public FDocumentData DocumentData = null;
			public bool bOptimizeHierarchy = true;

			public List<FBaseElementData> ChildElements = new List<FBaseElementData>();

			public FBaseElementData(
				ElementType InElementType, FDocumentData InDocumentData
			)
			{
				BaseElementType = InElementType;
				DocumentData = InDocumentData;
			}

			public FBaseElementData(FDatasmithFacadeActor InElementActor, FDatasmithFacadeMetaData InElementMetaData, FDocumentData InDocumentData)
			{
				ElementActor = InElementActor;
				ElementMetaData = InElementMetaData;
				DocumentData = InDocumentData;
			}

			public void SanitizeActorHierarchyNames()
			{
				string ElementName = ElementActor.GetName();

				if (ChildElements.Count > 0)
				{
					Dictionary<string, int> NameCountMap = new Dictionary<string, int>();
					Dictionary<string, int> NameUsageMap = new Dictionary<string, int>();

					// Count the number of times a name is reused by the Datasmith actor children.
					foreach (FBaseElementData ChildElement in ChildElements)
					{
						FDatasmithFacadeActor ChildActor = ChildElement.ElementActor;
						string ChildActorName = ChildActor.GetName();

						if (!NameCountMap.ContainsKey(ChildActorName))
						{
							NameCountMap.Add(ChildActorName, 0);
							NameUsageMap.Add(ChildActorName, 0);
						}

						NameCountMap[ChildActorName]++;
					}

					// Rename with a name made unique each child of the Datasmith actor.
					foreach (FBaseElementData ChildElement in ChildElements)
					{
						FDatasmithFacadeActor ChildActor = ChildElement.ElementActor;
						string ChildActorName = ChildActor.GetName();

						NameUsageMap[ChildActorName]++;

						// Build a new unique name for the Datasmith actor child.
						string NewChildActorName;
						if (NameCountMap[ChildActorName] > 1)
						{
							NewChildActorName = string.Format("{0}.{1}_{2}/{3}", ElementName, ChildActorName, NameUsageMap[ChildActorName], NameCountMap[ChildActorName]);
						}
						else
						{
							NewChildActorName = string.Format("{0}.{1}", ElementName, ChildActorName);
						}

						// Assign new unique name to the Datasmith actor child.
						ChildActor.SetName(NewChildActorName);

						// Make sure all the actor names are unique in the hierarchy of this Datasmith actor child.
						ChildElement.SanitizeActorHierarchyNames();
					}
				}

				// Hash the Datasmith actor name to shorten it.
				string HashedActorName = FDatasmithFacadeElement.GetStringHash(ElementName);
				ElementActor.SetName(HashedActorName);
				ElementMetaData.SetName(HashedActorName + "_DATA");
			}

			public bool IsSimpleActor()
			{
				FDatasmithFacadeActorMesh MeshActor = ElementActor as FDatasmithFacadeActorMesh;

				if (MeshActor != null && MeshActor.GetMeshName().Length == 0)
				{
					ElementActor = new FDatasmithFacadeActor(MeshActor.GetName());
					ElementActor.SetLabel(MeshActor.GetLabel());

					float X, Y, Z, W;
					MeshActor.GetTranslation(out X, out Y, out Z);
					ElementActor.SetTranslation(X, Y, Z);
					MeshActor.GetScale(out X, out Y, out Z);
					ElementActor.SetScale(X, Y, Z);
					MeshActor.GetRotation(out X, out Y, out Z, out W);
					ElementActor.SetRotation(X, Y, Z, W);

					ElementActor.SetLayer(MeshActor.GetLayer());

					for (int TagIndex = 0; TagIndex < MeshActor.GetTagsCount(); ++TagIndex)
					{
						ElementActor.AddTag(MeshActor.GetTag(TagIndex));
					}

					ElementActor.SetIsComponent(MeshActor.IsComponent());
					ElementActor.SetAsSelector(MeshActor.IsASelector());
					ElementActor.SetSelectionIndex(MeshActor.GetSelectionIndex());
					ElementActor.SetVisibility(MeshActor.GetVisibility());

					for (int ChildIndex = 0; ChildIndex < MeshActor.GetChildrenCount(); ++ChildIndex)
					{
						ElementActor.AddChild(MeshActor.GetChild(ChildIndex));
					}

					ElementMetaData.SetAssociatedElement(ElementActor);

					return true;
				}

				return !(ElementActor is FDatasmithFacadeActorMesh || ElementActor is FDatasmithFacadeActorLight	|| ElementActor is FDatasmithFacadeActorCamera);
			}

			private void AddToScene(FDatasmithFacadeScene InScene, FBaseElementData InElement, FBaseElementData InParent)
			{
				if (InParent == null)
				{
					InScene.AddActor(InElement.ElementActor);
				}
				else
				{
					InParent.ElementActor.AddChild(InElement.ElementActor);
				}

				if (!DocumentData.bSkipMetadataExport && InElement.ElementMetaData != null)
				{
					InScene.AddMetaData(InElement.ElementMetaData);
				}
			}

			public bool AddToScene(FDatasmithFacadeScene InScene, FBaseElementData InParentActor, bool bGlobalOptimizeHierarchy)
			{
				bool bHasChildSaved = false;
				bool bLocalOptimizeHierarchy = bOptimizeHierarchy & bGlobalOptimizeHierarchy;

				bool bIsSimpleActor = IsSimpleActor();

				if (bLocalOptimizeHierarchy && bIsSimpleActor && ChildElements.Count == 1)
				{
					FBaseElementData ChildElement = ChildElements[0];
					if (ChildElement.ElementActor.IsComponent() == true)
					{
						ChildElement.ElementActor.SetIsComponent(false);
					}

					return ChildElement.AddToScene(InScene, InParentActor, bGlobalOptimizeHierarchy);
				}

				foreach (FBaseElementData CurrentChild in ChildElements)
				{
					bHasChildSaved |= CurrentChild.AddToScene(InScene, this, bGlobalOptimizeHierarchy);
				}

				if (!bLocalOptimizeHierarchy || !bIsSimpleActor || bHasChildSaved)
				{
					AddToScene(InScene, this, InParentActor);
					return true;
				}

				return false;
			}

			public void UpdateMeshName()
			{
				FDatasmithFacadeActorMesh MeshActor = ElementActor as FDatasmithFacadeActorMesh;
				MeshActor.SetMesh(ElementMesh.GetName());
				bOptimizeHierarchy = false;
			}
		}

		private class FElementData : FBaseElementData
		{
			public Element CurrentElement;
			public Transform MeshPointsTransform = null;

			private Stack<FBaseElementData> InstanceDataStack = new Stack<FBaseElementData>();

			public FElementData(
				Element InElement,
				Transform InWorldTransform,
				FDatasmithFacadeActorMesh InExistingActor,
				FDocumentData InDocumentData
			)
				: base(InElement.Document.GetElement(InElement.GetTypeId()) as ElementType, InDocumentData)
			{
				CurrentElement = InElement;

				// If element has location, use it as a transform in order to have better pivot placement.
				Transform PivotTransform = GetPivotTransform(CurrentElement);
				if (PivotTransform != null)
				{
					if (!InWorldTransform.IsIdentity)
					{
						InWorldTransform = InWorldTransform * PivotTransform;
					}
					else
					{
						InWorldTransform = PivotTransform;
					}

					if (CurrentElement.GetType() == typeof(Wall)
						|| CurrentElement.GetType() == typeof(ModelText)
						|| CurrentElement.GetType().IsSubclassOf(typeof(MEPCurve)))
					{
						MeshPointsTransform = PivotTransform.Inverse;
					}
				}
				if (InExistingActor != null)
				{
					ElementActor = InExistingActor;
				}

				// Create a new Datasmith mesh actor.
				InitializeElement(InWorldTransform, this);
			}

			// Compute orthonormal basis, given the X vector.
			static private void ComputeBasis(XYZ BasisX, ref XYZ BasisY, ref XYZ BasisZ)
			{
				BasisY = XYZ.BasisZ.CrossProduct(BasisX);
				if (BasisY.GetLength() < 0.0001)
				{
					// BasisX is aligned with Z, use dot product to take direction in account
					BasisY = BasisX.CrossProduct(BasisX.DotProduct(XYZ.BasisZ) * XYZ.BasisX).Normalize();
					BasisZ = BasisX.CrossProduct(BasisY).Normalize();
				}
				else
				{
					BasisY = BasisY.Normalize();
					BasisZ = BasisX.CrossProduct(BasisY).Normalize();
				}
			}

			private Transform GetPivotTransform(Element InElement)
			{
				if (InElement.Location == null || (InElement as FamilyInstance) != null)
				{
					return null;
				}

				XYZ Translation = null;
				XYZ BasisX = new XYZ();
				XYZ BasisY = new XYZ();
				XYZ BasisZ = new XYZ();

				// Get pivot translation

				if (InElement.Location.GetType() == typeof(LocationCurve))
				{
					LocationCurve CurveLocation = InElement.Location as LocationCurve;
					if (CurveLocation.Curve != null && CurveLocation.Curve.IsBound)
					{
						Translation = CurveLocation.Curve.GetEndPoint(0);
					}
				}
				else if (InElement.Location.GetType() == typeof(LocationPoint))
				{
					Translation = (InElement.Location as LocationPoint).Point;
				}
				else if (InElement.GetType() == typeof(Railing))
				{
					// Railings don't have valid location, so instead we need to get location from its path.
					IList<Curve> Paths = (InElement as Railing).GetPath();
					if (Paths.Count > 0 && Paths[0].IsBound)
					{
						Translation = Paths[0].GetEndPoint(0);
					}
				}

				if (Translation == null)
				{
					return null; // Cannot get valid translation
				}

				// Get pivot basis

				if (InElement.GetType() == typeof(Wall))
				{
					BasisY = (InElement as Wall).Orientation.Normalize();
					BasisX = BasisY.CrossProduct(XYZ.BasisZ).Normalize();
					BasisZ = BasisX.CrossProduct(BasisY).Normalize();
				}
				else if (InElement.GetType() == typeof(Railing))
				{
					IList<Curve> Paths = (InElement as Railing).GetPath();
					if (Paths.Count > 0)
					{
						Curve FirstPath = Paths[0];
						if (FirstPath.GetType() == typeof(Line))
						{
							BasisX = (FirstPath as Line).Direction.Normalize();
							ComputeBasis(BasisX, ref BasisY, ref BasisZ);
						}
						else if (FirstPath.GetType() == typeof(Arc) && FirstPath.IsBound)
						{
							Transform Derivatives = (FirstPath as Arc).ComputeDerivatives(0f, true);
							BasisX = Derivatives.BasisX.Normalize();
							BasisY = Derivatives.BasisY.Normalize();
							BasisZ = Derivatives.BasisZ.Normalize();
						}
					}
				}
				else if (InElement.GetType() == typeof(ModelText))
				{
					// Model text has no direction information!
					BasisX = XYZ.BasisX;
					BasisY = XYZ.BasisY;
					BasisZ = XYZ.BasisZ;
				}
				else if (InElement.GetType() == typeof(FlexDuct))
				{
					BasisX = (InElement as FlexDuct).StartTangent;
					ComputeBasis(BasisX, ref BasisY, ref BasisZ);
				}
				else if (InElement.GetType() == typeof(FlexPipe))
				{
					BasisX = (InElement as FlexPipe).StartTangent;
					ComputeBasis(BasisX, ref BasisY, ref BasisZ);
				}
				else if (InElement.Location.GetType() == typeof(LocationCurve))
				{
					LocationCurve CurveLocation = InElement.Location as LocationCurve;

					if (CurveLocation.Curve.GetType() == typeof(Line))
					{
						BasisX = (CurveLocation.Curve as Line).Direction;
						ComputeBasis(BasisX, ref BasisY, ref BasisZ);
					}
					else if (CurveLocation.Curve.IsBound)
					{
						Transform Derivatives = CurveLocation.Curve.ComputeDerivatives(0f, true);
						BasisX = Derivatives.BasisX.Normalize();
						BasisY = Derivatives.BasisY.Normalize();
						BasisZ = Derivatives.BasisZ.Normalize();
					}
					else
					{
						return null;
					}
				}
				else
				{
					return null; // Cannot get valid basis
				}

				Transform PivotTransform = Transform.CreateTranslation(Translation);
				PivotTransform.BasisX = BasisX;
				PivotTransform.BasisY = BasisY;
				PivotTransform.BasisZ = BasisZ;

				return PivotTransform;
			}

			public void PushInstance(
				ElementType InInstanceType,
				Transform InWorldTransform
			)
			{
				FBaseElementData InstanceData = new FBaseElementData(InInstanceType, DocumentData);

				InitializeElement(InWorldTransform, InstanceData);

				InstanceDataStack.Push(InstanceData);

				// The Datasmith instance actor is a component in the hierarchy.
				InstanceData.ElementActor.SetIsComponent(true);
			}

			public FBaseElementData PopInstance()
			{
				return InstanceDataStack.Pop();
			}

			public void AddLightActor(
				Transform InWorldTransform,
				Asset InLightAsset
			)
			{
				// Create a new Datasmith light actor.
				// Hash the Datasmith light actor name to shorten it.
				string HashedActorName = FDatasmithFacadeElement.GetStringHash("A:" + GetActorName());
				FDatasmithFacadeActorLight LightActor = FDatasmithRevitLight.CreateLightActor(CurrentElement, HashedActorName);
				LightActor.SetLabel(GetActorLabel());

				// Set the world transform of the Datasmith light actor.
				FDocumentData.SetActorTransform(InWorldTransform, LightActor);

				// Set the base properties of the Datasmith light actor.
				string LayerName = Category.GetCategory(CurrentElement.Document, BuiltInCategory.OST_LightingFixtureSource)?.Name ?? "Light Sources";
				SetActorProperties(LayerName, LightActor);

				FDatasmithFacadeMetaData LightMetaData = GetActorMetaData(LightActor);

				// Set the Datasmith light actor layer to its predefined name.
				string CategoryName = Category.GetCategory(CurrentElement.Document, BuiltInCategory.OST_LightingFixtureSource)?.Name ?? "Light Sources";
				LightActor.SetLayer(CategoryName);

				// Set the specific properties of the Datasmith light actor.
				FDatasmithRevitLight.SetLightProperties(InLightAsset, CurrentElement, LightActor);

				// Add the light actor to the Datasmith actor hierarchy.
				AddChildActor(LightActor, LightMetaData, false);
			}

			public FDatasmithFacadeMesh AddRPCActor(
				Transform InWorldTransform,
				Asset InRPCAsset,
				int InMaterialIndex
			)
			{
				// Create a new Datasmith RPC mesh.
				// Hash the Datasmith RPC mesh name to shorten it.
				string HashedName = FDatasmithFacadeElement.GetStringHash("M:" + GetActorName());
				FDatasmithFacadeMesh RPCMesh = new FDatasmithFacadeMesh(HashedName);
				RPCMesh.SetLabel(GetActorLabel());

				Transform AffineTransform = Transform.Identity;

				LocationPoint RPCLocationPoint = CurrentElement.Location as LocationPoint;

				if (RPCLocationPoint != null)
				{
					if (RPCLocationPoint.Rotation != 0.0)
					{
						AffineTransform = AffineTransform.Multiply(Transform.CreateRotation(XYZ.BasisZ, -RPCLocationPoint.Rotation));
						AffineTransform = AffineTransform.Multiply(Transform.CreateTranslation(RPCLocationPoint.Point.Negate()));
					}
					else
					{
						AffineTransform = Transform.CreateTranslation(RPCLocationPoint.Point.Negate());
					}
				}

				GeometryElement RPCGeometryElement = CurrentElement.get_Geometry(new Options());

				foreach (GeometryObject RPCGeometryObject in RPCGeometryElement)
				{
					GeometryInstance RPCGeometryInstance = RPCGeometryObject as GeometryInstance;

					if (RPCGeometryInstance != null)
					{
						GeometryElement RPCInstanceGeometry = RPCGeometryInstance.GetInstanceGeometry();

						foreach (GeometryObject RPCInstanceGeometryObject in RPCInstanceGeometry)
						{
							Mesh RPCInstanceGeometryMesh = RPCInstanceGeometryObject as Mesh;

							if (RPCInstanceGeometryMesh == null || RPCInstanceGeometryMesh.NumTriangles < 1)
							{
								continue;
							}

							// RPC geometry does not have normals nor UVs available through the Revit Mesh interface.
							int InitialVertexCount = RPCMesh.GetVertexCount();
							int TriangleCount = RPCInstanceGeometryMesh.NumTriangles;

							// Add the RPC geometry vertices to the Datasmith RPC mesh.
							foreach (XYZ Vertex in RPCInstanceGeometryMesh.Vertices)
							{
								XYZ PositionedVertex = AffineTransform.OfPoint(Vertex);
								RPCMesh.AddVertex((float)PositionedVertex.X, (float)PositionedVertex.Y, (float)PositionedVertex.Z);
							}

							// Add the RPC geometry triangles to the Datasmith RPC mesh.
							for (int TriangleNo = 0; TriangleNo < TriangleCount; TriangleNo++)
							{
								MeshTriangle Triangle = RPCInstanceGeometryMesh.get_Triangle(TriangleNo);

								try
								{
									int Index0 = Convert.ToInt32(Triangle.get_Index(0));
									int Index1 = Convert.ToInt32(Triangle.get_Index(1));
									int Index2 = Convert.ToInt32(Triangle.get_Index(2));

									// Add triangles for both the front and back faces.
									RPCMesh.AddTriangle(InitialVertexCount + Index0, InitialVertexCount + Index1, InitialVertexCount + Index2, InMaterialIndex);
									RPCMesh.AddTriangle(InitialVertexCount + Index2, InitialVertexCount + Index1, InitialVertexCount + Index0, InMaterialIndex);
								}
								catch (OverflowException)
								{
									continue;
								}
							}
						}
					}
				}

				// Create a new Datasmith RPC mesh actor.
				// Hash the Datasmith RPC mesh actor name to shorten it.
				string HashedActorName = FDatasmithFacadeElement.GetStringHash("A:" + GetActorName());
				FDatasmithFacadeActor FacadeActor;
				if (RPCMesh.GetVertexCount() > 0 && RPCMesh.GetTriangleCount() > 0)
				{
					FDatasmithFacadeActorMesh RPCMeshActor = new FDatasmithFacadeActorMesh(HashedActorName);
					RPCMeshActor.SetMesh(RPCMesh.GetName());
					FacadeActor = RPCMeshActor;
				}
				else
				{
					//Create an empty actor instead of a static mesh actor with no mesh.
					FacadeActor = new FDatasmithFacadeActor(HashedActorName);
				}
				FacadeActor.SetLabel(GetActorLabel());

				// Set the world transform of the Datasmith RPC mesh actor.
				FDocumentData.SetActorTransform(InWorldTransform, FacadeActor);

				// Set the base properties of the Datasmith RPC mesh actor.
				string LayerName = GetCategoryName();
				SetActorProperties(LayerName, FacadeActor);

				// Add a Revit element RPC tag to the Datasmith RPC mesh actor.
				FacadeActor.AddTag("Revit.Element.RPC");

				// Add some Revit element RPC metadata to the Datasmith RPC mesh actor.
				AssetProperty RPCTypeId = InRPCAsset.FindByName("RPCTypeId");
				AssetProperty RPCFilePath = InRPCAsset.FindByName("RPCFilePath");

				FDatasmithFacadeMetaData ElementMetaData = new FDatasmithFacadeMetaData(FacadeActor.GetName() + "_DATA");
				ElementMetaData.SetLabel(FacadeActor.GetLabel());
				ElementMetaData.SetAssociatedElement(FacadeActor);

				if (RPCTypeId != null)
				{
					ElementMetaData.AddPropertyString("Type*RPCTypeId", (RPCTypeId as AssetPropertyString).Value);
				}

				if (RPCFilePath != null)
				{
					ElementMetaData.AddPropertyString("Type*RPCFilePath", (RPCFilePath as AssetPropertyString).Value);
				}

				// Add the RPC mesh actor to the Datasmith actor hierarchy.
				AddChildActor(FacadeActor, ElementMetaData, false);

				return RPCMesh;
			}

			public void AddChildActor(
				FBaseElementData InChildActor
			)
			{
				FBaseElementData ParentElement = (InstanceDataStack.Count == 0) ? this : InstanceDataStack.Peek();

				ParentElement.ChildElements.Add(InChildActor);
			}

			public void AddChildActor(
				FDatasmithFacadeActor ChildActor, FDatasmithFacadeMetaData MetaData, bool bOptimizeHierarchy
			)
			{
				FBaseElementData ElementData = new FBaseElementData(ChildActor, MetaData, DocumentData);
				ElementData.bOptimizeHierarchy = bOptimizeHierarchy;

				FBaseElementData Parent = (InstanceDataStack.Count == 0) ? this : InstanceDataStack.Peek();

				Parent.ChildElements.Add(ElementData);
			}

			private void InitializeElement(
					Transform InWorldTransform,
					FBaseElementData InElement
			)
			{
				// Create a new Datasmith mesh.
				// Hash the Datasmith mesh name to shorten it.
				string HashedMeshName = FDatasmithFacadeElement.GetStringHash("M:" + GetActorName());
				InElement.ElementMesh = new FDatasmithFacadeMesh(HashedMeshName);
				InElement.ElementMesh.SetLabel(GetActorLabel());

				if(InElement.ElementActor == null)
				{
					// Create a new Datasmith mesh actor.
					// Hash the Datasmith mesh actor name to shorten it.
					string HashedActorName = FDatasmithFacadeElement.GetStringHash("A:" + GetActorName());
					InElement.ElementActor = new FDatasmithFacadeActorMesh(HashedActorName);
					InElement.ElementActor.SetLabel(GetActorLabel());
				}

				// Set the world transform of the Datasmith mesh actor.
				FDocumentData.SetActorTransform(InWorldTransform, InElement.ElementActor);

				// Set the base properties of the Datasmith mesh actor.
				string LayerName = GetCategoryName();
				SetActorProperties(LayerName, InElement.ElementActor);

				InElement.ElementMetaData = GetActorMetaData(InElement.ElementActor);
			}

			public string GetCategoryName()
			{
				return BaseElementType?.Category?.Name ?? CurrentElement.Category?.Name;
			}

			public bool IgnoreElementGeometry()
			{
				// Ignore elements that have unwanted geometry, such as level symbols.
				return (BaseElementType as LevelType) != null;
			}

			public FDatasmithFacadeMesh GetCurrentMesh()
			{
				if (InstanceDataStack.Count == 0)
				{
					return ElementMesh;
				}
				else
				{
					return InstanceDataStack.Peek().ElementMesh;
				}
			}

			public FDatasmithFacadeMesh PeekInstancedMesh()
			{
				if (InstanceDataStack.Count == 0)
				{
					return null;
				}
				else
				{
					return InstanceDataStack.Peek().ElementMesh;
				}
			}

			public FBaseElementData GetCurrentActor()
			{
				if (InstanceDataStack.Count == 0)
				{
					return this;
				}
				else
				{
					return InstanceDataStack.Peek();
				}
			}

			public void Log(
				FDatasmithFacadeLog InDebugLog,
				string InLinePrefix,
				int InLineIndentation
			)
			{
				if (InDebugLog != null)
				{
					if (InLineIndentation < 0)
					{
						InDebugLog.LessIndentation();
					}

					Element SourceElement = (InstanceDataStack.Count == 0) ? CurrentElement : InstanceDataStack.Peek().BaseElementType;

					InDebugLog.AddLine($"{InLinePrefix} {SourceElement.Id.IntegerValue} '{SourceElement.Name}' {SourceElement.GetType()}: '{GetActorLabel()}'");

					if (InLineIndentation > 0)
					{
						InDebugLog.MoreIndentation();
					}
				}
			}

			private string GetActorName()
			{
				string DocumentName = Path.GetFileNameWithoutExtension(CurrentElement.Document.PathName);

				if (InstanceDataStack.Count == 0)
				{
					return $"{DocumentName}:{CurrentElement.UniqueId}";
				}
				else
				{
					return $"{DocumentName}:{InstanceDataStack.Peek().BaseElementType.UniqueId}";
				}
			}

			private string GetActorLabel()
			{
				string CategoryName = GetCategoryName();
				string FamilyName = BaseElementType?.FamilyName;
				string TypeName = BaseElementType?.Name;
				string InstanceName = (InstanceDataStack.Count > 1) ? InstanceDataStack.Peek().BaseElementType?.Name : null;

				string ActorLabel = "";

				if (CurrentElement as Level != null)
				{
					ActorLabel += string.IsNullOrEmpty(FamilyName) ? "" : FamilyName + "*";
					ActorLabel += string.IsNullOrEmpty(TypeName) ? "" : TypeName + "*";
					ActorLabel += CurrentElement.Name;
				}
				else
				{
					ActorLabel += string.IsNullOrEmpty(CategoryName) ? "" : CategoryName + "*";
					ActorLabel += string.IsNullOrEmpty(FamilyName) ? "" : FamilyName + "*";
					ActorLabel += string.IsNullOrEmpty(TypeName) ? CurrentElement.Name : TypeName;
					ActorLabel += string.IsNullOrEmpty(InstanceName) ? "" : "*" + InstanceName;
				}

				return ActorLabel;
			}

			private void SetActorProperties(
				string InLayerName,
				FDatasmithFacadeActor IOActor
			)
			{
				// Set the Datasmith actor layer to the element type category name.
				IOActor.SetLayer(InLayerName);

				// Add the Revit element ID and Unique ID tags to the Datasmith actor.
				IOActor.AddTag($"Revit.Element.Id.{CurrentElement.Id.IntegerValue}");
				IOActor.AddTag($"Revit.Element.UniqueId.{CurrentElement.UniqueId}");

				// For an hosted Revit family instance, add the host ID, Unique ID and Mirrored/Flipped flags as tags to the Datasmith actor.
				FamilyInstance CurrentFamilyInstance = CurrentElement as FamilyInstance;
				if (CurrentFamilyInstance != null)
				{
					IOActor.AddTag($"Revit.DB.FamilyInstance.Mirrored.{CurrentFamilyInstance.Mirrored}");
					IOActor.AddTag($"Revit.DB.FamilyInstance.HandFlipped.{CurrentFamilyInstance.HandFlipped}");
					IOActor.AddTag($"Revit.DB.FamilyInstance.FaceFlipped.{CurrentFamilyInstance.FacingFlipped}");

					if (CurrentFamilyInstance.Host != null)
					{
						IOActor.AddTag($"Revit.Host.Id.{CurrentFamilyInstance.Host.Id.IntegerValue}");
						IOActor.AddTag($"Revit.Host.UniqueId.{CurrentFamilyInstance.Host.UniqueId}");
					}
				}
			}

			private FDatasmithFacadeMetaData GetActorMetaData(FDatasmithFacadeActor IOActor)
			{
				FDatasmithFacadeMetaData ElementMetaData = new FDatasmithFacadeMetaData(IOActor.GetName() + "_DATA");
				ElementMetaData.SetLabel(IOActor.GetLabel());
				ElementMetaData.SetAssociatedElement(IOActor);

				// Add the Revit element category name metadata to the Datasmith actor.
				string CategoryName = GetCategoryName();
				if (!string.IsNullOrEmpty(CategoryName))
				{
					ElementMetaData.AddPropertyString("Element*Category", CategoryName);
				}

				// Add the Revit element family name metadata to the Datasmith actor.
				string FamilyName = BaseElementType?.FamilyName;
				if (!string.IsNullOrEmpty(FamilyName))
				{
					ElementMetaData.AddPropertyString("Element*Family", FamilyName);
				}

				// Add the Revit element type name metadata to the Datasmith actor.
				string TypeName = BaseElementType?.Name;
				if (!string.IsNullOrEmpty(TypeName))
				{
					ElementMetaData.AddPropertyString("Element*Type", TypeName);
				}

				// Add Revit element metadata to the Datasmith actor.
				FDocumentData.AddActorMetadata(CurrentElement, "Element*", ElementMetaData);

				if (BaseElementType != null)
				{
					// Add Revit element type metadata to the Datasmith actor.
					FDocumentData.AddActorMetadata(BaseElementType, "Type*", ElementMetaData);
				}

				return ElementMetaData;
			}
		}

		private FExportData				CachedExportData = null;
		private FExportData				CurrentExportData = new FExportData();

		private Document				CurrentDocument;
		private Stack<FElementData>		ElementDataStack = new Stack<FElementData>();
		private string					CurrentMaterialDataKey = null;
		private int						LatestMaterialIndex = 0;
		private List<string>			MessageList = null;

		private HashSet<int>			ModifiedElementsSet = new HashSet<int>();

		public FDatasmithFacadeScene	DatasmithScene = null;

		public bool						bSkipMetadataExport = false;

		public FDocumentData(
			Document InDocument,
			FDatasmithFacadeScene InScene,
			ref List<string> InMessageList,
			FExportData Cache = null
		)
		{
			CachedExportData = Cache;
			CurrentDocument = InDocument;
			MessageList = InMessageList;
			DatasmithScene = InScene;
		}

		private bool IsActorCached(ElementId ElemId)
		{
			if (CachedExportData != null)
			{
				return CachedExportData.ActorMap.ContainsKey(ElemId);
			}
			return false;
		}

		private bool IsActorModified(ElementId ElemId)
		{
			if (CachedExportData != null)
			{
				return CachedExportData.ModifiedActorSet.Contains(ElemId);
			}
			return false;
		}

		public void OnElementModified(ElementId ElemId)
		{
			ModifiedElementsSet.Add(ElemId.IntegerValue);
		}

		public void OnElementAdded(ElementId ElemId)
		{
			//todo
			/*
			if (DeletedElements.Contains(id))
			{
				// Element was deleted and added back (undo) before being synced (sent to datasmith),
				// so just remove it from deleted
				DeletedElements.Remove(id);
			}
			else
			{
				// New element of supported type
				AddedElements.Add(id);
			}
			*/
		}

		public void OnElementDeleted(ElementId ElemId)
		{
			//todo

			/*
			if (AddedElements.Contains(id))
			{
				// Element was added and deleted before being synced (sent to datasmith),
				// don't add it to the deleted elements
				AddedElements.Remove(id);
			}
			else if (SyncedElements.Contains(id))
			{
				// Flag existing datasmith element for removal
				DeletedElements.Add(id);
			}
			*/
		}

		public Document GetDocument()
		{
			return CurrentDocument;
		}

		public Element GetElement(
			ElementId InElementId
		)
		{
			return (InElementId != ElementId.InvalidElementId) ? CurrentDocument.GetElement(InElementId) : null;
		}

		public bool ContainsMesh(string MeshName)
		{
			return CurrentExportData.MeshMap.ContainsKey(MeshName);
		}

		public bool PushElement(
			Element InElement,
			Transform InWorldTransform
		)
		{
			FDatasmithFacadeActorMesh ExistingActor = null;

			if (IsActorCached(InElement.Id))
			{
				if (IsActorModified(InElement.Id))
				{
					ExistingActor = CachedExportData.ActorMap[InElement.Id].ElementActor as FDatasmithFacadeActorMesh;

					// In case of instancing, we need to remove the child instances that will be re-created.
					if (ExistingActor.GetChildrenCount() > 0)
					{
						List<FDatasmithFacadeActor> ChildToRemove = new List<FDatasmithFacadeActor>();
						
						for(int ChildIndex = 0; ChildIndex < ExistingActor.GetChildrenCount(); ++ChildIndex)
						{
							FDatasmithFacadeActor Child = ExistingActor.GetChild(ChildIndex);
							if(Child.GetTagsCount() > 0)
							{
								for(int TagIndex = 0; TagIndex < Child.GetTagsCount(); ++TagIndex)
								{
									if(Child.GetTag(TagIndex) == "Revit.Element.IsInstance")
									{
										ChildToRemove.Add(Child);
										break;
									}
								}
							}
						}

						foreach(FDatasmithFacadeActor Child in ChildToRemove)
						{
							ExistingActor.RemoveChild(Child);
						}
					}
				}
				else
				{
					return false; // We have up to date cache for this element.
				}
			}

			ElementDataStack.Push(new FElementData(InElement, InWorldTransform, ExistingActor, this));
			ElementDataStack.Peek().ElementActor.AddTag("IsElement");

			return true;
		}

		public void PopElement()
		{
			FElementData ElementData = ElementDataStack.Pop();

			FDatasmithFacadeMesh ElementMesh = ElementData.ElementMesh;

			if(ElementMesh.GetVertexCount() > 0 && ElementMesh.GetTriangleCount() > 0)
			{
				ElementData.UpdateMeshName();
			}

			CollectMesh(ElementMesh);

			if (IsActorModified(ElementData.CurrentElement.Id))
			{
				ElementData.ModifiedFlags = EActorModifiedFlags.ActorModifiedProperties;
			}

			if (ElementDataStack.Count == 0)
			{
				ElementId ElemId = ElementData.CurrentElement.Id;

				if (CurrentExportData.ActorMap.ContainsKey(ElemId))
				{
					// Handle the spurious case of Revit Custom Exporter calling back more than once for the same element.
					// These extra empty actors will be cleaned up later by the Datasmith actor hierarchy optimization.
					CurrentExportData.ActorMap[ElemId].ChildElements.Add(ElementData);
				}
				else
				{
					// Collect the element mesh actor into the Datasmith actor dictionary.
					CurrentExportData.ActorMap[ElemId] = ElementData;
				}
			}
			else
			{
				// Add the element mesh actor to the Datasmith actor hierarchy.
				ElementDataStack.Peek().AddChildActor(ElementData);
			}
		}

		private static FDatasmithFacadeActor DuplicateBaseActor(FDatasmithFacadeActor SourceActor)
		{
			FDatasmithFacadeActor CloneActor = new FDatasmithFacadeActor(SourceActor.GetName());
			CloneActor.SetLabel(SourceActor.GetLabel());

			float X, Y, Z, W;
			SourceActor.GetTranslation(out X, out Y, out Z);
			CloneActor.SetTranslation(X, Y, Z);
			SourceActor.GetScale(out X, out Y, out Z);
			CloneActor.SetScale(X, Y, Z);
			SourceActor.GetRotation(out X, out Y, out Z, out W);
			CloneActor.SetRotation(X, Y, Z, W);

			CloneActor.SetLayer(SourceActor.GetLayer());

			for (int TagIndex = 0; TagIndex < SourceActor.GetTagsCount(); ++TagIndex)
			{
				CloneActor.AddTag(SourceActor.GetTag(TagIndex));
			}

			CloneActor.SetIsComponent(SourceActor.IsComponent());
			CloneActor.SetAsSelector(SourceActor.IsASelector());
			CloneActor.SetSelectionIndex(SourceActor.GetSelectionIndex());
			CloneActor.SetVisibility(SourceActor.GetVisibility());

			for (int ChildIndex = 0; ChildIndex < SourceActor.GetChildrenCount(); ++ChildIndex)
			{
				CloneActor.AddChild(SourceActor.GetChild(ChildIndex));
			}

			return CloneActor;
		}

		public void PushInstance(
			ElementType InInstanceType,
			Transform InWorldTransform
		)
		{
			ElementDataStack.Peek().PushInstance(InInstanceType, InWorldTransform);
		}

		public void PopInstance()
		{
			FElementData CurrentElement = ElementDataStack.Peek();
			FBaseElementData InstanceData = CurrentElement.PopInstance();
			FDatasmithFacadeMesh ElementMesh = InstanceData.ElementMesh;

			if (ContainsMesh(ElementMesh.GetName()) || (ElementMesh.GetVertexCount() > 0 && ElementMesh.GetTriangleCount() > 0))
			{
				InstanceData.UpdateMeshName();
			}

			// Collect the element Datasmith mesh into the mesh dictionary.
			CollectMesh(ElementMesh);

			// Add the instance mesh actor to the Datasmith actor hierarchy.
			CurrentElement.AddChildActor(InstanceData);
		}

		public void AddLocationActors(
			Transform InWorldTransform
		)
		{
			// Add a new Datasmith placeholder actor for this document site location.
			AddSiteLocation(CurrentDocument.SiteLocation);

			// Add new Datasmith placeholder actors for the project base point and survey points.
			// A project has one base point and at least one survey point. Linked documents also have their own points.
			AddPointLocations(InWorldTransform);
		}

		public void AddLightActor(
			Transform InWorldTransform,
			Asset InLightAsset
		)
		{
			ElementDataStack.Peek().AddLightActor(InWorldTransform, InLightAsset);
		}

		public void AddRPCActor(
			Transform InWorldTransform,
			Asset InRPCAsset
		)
		{
			// Create a simple fallback material for the RPC mesh.
			string RPCCategoryName = ElementDataStack.Peek().GetCategoryName();
			bool isRPCPlant = !string.IsNullOrEmpty(RPCCategoryName) && RPCCategoryName == Category.GetCategory(CurrentDocument, BuiltInCategory.OST_Planting)?.Name;
			string RPCMaterialName = isRPCPlant ? "RPC_Plant" : "RPC_Material";

			if (!CurrentExportData.MaterialDataMap.ContainsKey(RPCMaterialName))
			{
				// Color reference: https://www.color-hex.com/color-palette/70002
				Color RPCColor = isRPCPlant ? /* green */ new Color(88, 126, 96) : /* gray */ new Color(128, 128, 128);

				// Keep track of a new RPC master material.
				CurrentExportData.MaterialDataMap[RPCMaterialName] = new FMaterialData(RPCMaterialName, RPCColor, ++LatestMaterialIndex);
			}

			FMaterialData RPCMaterialData = CurrentExportData.MaterialDataMap[RPCMaterialName];

			FDatasmithFacadeMesh RPCMesh = ElementDataStack.Peek().AddRPCActor(InWorldTransform, InRPCAsset, RPCMaterialData.MaterialIndex);

			// Add the RPC master material name to the dictionary of material names utilized by the RPC mesh.
			RPCMesh.AddMaterial(RPCMaterialData.MaterialIndex, RPCMaterialData.MasterMaterial.GetName());

			// Collect the RPC mesh into the Datasmith mesh dictionary.
			CollectMesh(RPCMesh);
		}

		public bool SetMaterial(
			MaterialNode InMaterialNode,
			IList<string> InExtraTexturePaths
		)
		{
			Material CurrentMaterial = GetElement(InMaterialNode.MaterialId) as Material;

			CurrentMaterialDataKey = FMaterialData.GetMaterialName(InMaterialNode, CurrentMaterial);

			if (!CurrentExportData.MaterialDataMap.ContainsKey(CurrentMaterialDataKey))
			{
				// Keep track of a new Datasmith master material.
				CurrentExportData.MaterialDataMap[CurrentMaterialDataKey] = new FMaterialData(InMaterialNode, CurrentMaterial, ++LatestMaterialIndex, InExtraTexturePaths);

				// A new Datasmith master material was created.
				return true;
			}

			// No new Datasmith master material created.
			return false;
		}

		public bool IgnoreElementGeometry()
		{
			bool bIgnore = ElementDataStack.Peek().IgnoreElementGeometry();

			if (!bIgnore)
			{
				// Check for instanced meshes.
				FDatasmithFacadeMesh Mesh = ElementDataStack.Peek().PeekInstancedMesh();
				if (Mesh != null)
				{
					bIgnore = CurrentExportData.MeshMap.ContainsKey(Mesh.GetName());
				}
			}

			return bIgnore;
		}

		public FDatasmithFacadeMesh GetCurrentMesh()
		{
			return ElementDataStack.Peek().GetCurrentMesh();
		}

		public Transform GetCurrentMeshPointsTransform()
		{
			return ElementDataStack.Peek().MeshPointsTransform;
		}

		public int GetCurrentMaterialIndex()
		{
			if (CurrentExportData.MaterialDataMap.ContainsKey(CurrentMaterialDataKey))
			{
				FMaterialData MaterialData = CurrentExportData.MaterialDataMap[CurrentMaterialDataKey];

				// Add the current Datasmith master material name to the dictionary of material names utilized by the Datasmith mesh being processed.
				GetCurrentMesh().AddMaterial(MaterialData.MaterialIndex, MaterialData.MasterMaterial.GetName());

				// Return the index of the current material.
				return MaterialData.MaterialIndex;
			}

			return 0;
		}

		public FBaseElementData GetCurrentActor()
		{
			return ElementDataStack.Peek().GetCurrentActor();
		}

		public void WrapupLink(
			FDatasmithFacadeScene InDatasmithScene,
			FBaseElementData InLinkActor
		)
		{
			// TODO Cache collected actors!!!

			// Add the collected meshes from the Datasmith mesh dictionary to the Datasmith scene.
			AddCollectedMeshes(InDatasmithScene);

			// Factor in the Datasmith actor hierarchy the Revit document host hierarchy.
			AddHostHierarchy();

			// Factor in the Datasmith actor hierarchy the Revit document level hierarchy.
			AddLevelHierarchy();

			if (CurrentExportData.ActorMap.Count > 0)
			{
				// Prevent the Datasmith link actor from being removed by optimization.
				InLinkActor.bOptimizeHierarchy = false;

				// Add the collected actors from the Datasmith actor dictionary as children of the Datasmith link actor.
				foreach (FBaseElementData CollectedActor in CurrentExportData.ActorMap.Values)
				{
					InLinkActor.ChildElements.Add(CollectedActor);
				}
			}

			// Add the collected master materials from the material data dictionary to the Datasmith scene.
			AddCollectedMaterials(InDatasmithScene);
		}

		public void WrapupScene(
			FDatasmithFacadeScene InDatasmithScene,
			bool bOptimizeHierarchy
		)
		{
			// Add the collected meshes from the Datasmith mesh dictionary to the Datasmith scene.
			AddCollectedMeshes(InDatasmithScene);

			// Factor in the Datasmith actor hierarchy the Revit document host hierarchy.
			AddHostHierarchy();

			// Factor in the Datasmith actor hierarchy the Revit document level hierarchy.
			AddLevelHierarchy();

			// Add the collected actors from the Datasmith actor dictionary to the Datasmith scene.
			foreach (FBaseElementData CollectedActor in CurrentExportData.ActorMap.Values)
			{
				// Make sure all the actor names are unique and persistent in the Datasmith actor hierarchy.
				CollectedActor.SanitizeActorHierarchyNames();

				CollectedActor.AddToScene(InDatasmithScene, null, bOptimizeHierarchy);
			}

			// Add the collected master materials from the material data dictionary to the Datasmith scene.
			AddCollectedMaterials(InDatasmithScene);
		}

		public void LogElement(
			FDatasmithFacadeLog InDebugLog,
			string InLinePrefix,
			int InLineIndentation
		)
		{
			ElementDataStack.Peek().Log(InDebugLog, InLinePrefix, InLineIndentation);
		}

		public void LogMaterial(
			MaterialNode InMaterialNode,
			FDatasmithFacadeLog InDebugLog,
			string InLinePrefix
		)
		{
			if (CurrentExportData.MaterialDataMap.ContainsKey(CurrentMaterialDataKey))
			{
				CurrentExportData.MaterialDataMap[CurrentMaterialDataKey].Log(InMaterialNode, InDebugLog, InLinePrefix);
			}
		}

		private void AddSiteLocation(
			SiteLocation InSiteLocation
		)
		{
			if (InSiteLocation == null || !InSiteLocation.IsValidObject || IsActorCached(InSiteLocation.Id))
			{
				return;
			}

			// Create a new Datasmith placeholder actor for the site location.
			// Hash the Datasmith placeholder actor name to shorten it.
			string NameHash = FDatasmithFacadeElement.GetStringHash("SiteLocation");
			FDatasmithFacadeActor SiteLocationActor = new FDatasmithFacadeActor(NameHash);
			SiteLocationActor.SetLabel("Site Location");

			// Set the Datasmith placeholder actor layer to the site location category name.
			SiteLocationActor.SetLayer(InSiteLocation.Category.Name);

			// Add the Revit element ID and Unique ID tags to the Datasmith placeholder actor.
			SiteLocationActor.AddTag($"Revit.Element.Id.{InSiteLocation.Id.IntegerValue}");
			SiteLocationActor.AddTag($"Revit.Element.UniqueId.{InSiteLocation.UniqueId}");

			// Add a Revit element site location tag to the Datasmith placeholder actor.
			SiteLocationActor.AddTag("Revit.Element.SiteLocation");

			FDatasmithFacadeMetaData SiteLocationMetaData = new FDatasmithFacadeMetaData(SiteLocationActor.GetName() + "_DATA");
			SiteLocationMetaData.SetLabel(SiteLocationActor.GetLabel());
			SiteLocationMetaData.SetAssociatedElement(SiteLocationActor);

			// Add site location metadata to the Datasmith placeholder actor.
			const double RadiansToDegrees = 180.0 / Math.PI;
			SiteLocationMetaData.AddPropertyFloat("SiteLocation*Latitude", (float)(InSiteLocation.Latitude * RadiansToDegrees));
			SiteLocationMetaData.AddPropertyFloat("SiteLocation*Longitude", (float)(InSiteLocation.Longitude * RadiansToDegrees));
			SiteLocationMetaData.AddPropertyFloat("SiteLocation*Elevation", (float)InSiteLocation.Elevation);
			SiteLocationMetaData.AddPropertyFloat("SiteLocation*TimeZone", (float)InSiteLocation.TimeZone);
			SiteLocationMetaData.AddPropertyString("SiteLocation*Place", InSiteLocation.PlaceName);

			// Collect the site location placeholder actor into the Datasmith actor dictionary.
			FBaseElementData ElementData = new FBaseElementData(SiteLocationActor, SiteLocationMetaData, this);
			// Prevent the Datasmith placeholder actor from being removed by optimization.
			ElementData.bOptimizeHierarchy = false;
			CurrentExportData.ActorMap[InSiteLocation.Id] = ElementData;
		}

		private void AddPointLocations(
			Transform InWorldTransform
		)
		{
			FilteredElementCollector Collector = new FilteredElementCollector(CurrentDocument);
			ICollection<Element> PointLocations = Collector.OfClass(typeof(BasePoint)).ToElements();

			foreach (Element PointLocation in PointLocations)
			{
				BasePoint BasePointLocation = PointLocation as BasePoint;

				if (BasePointLocation != null)
				{
					if (IsActorCached(BasePointLocation.Id))
					{
						continue;
					}

					// Since BasePoint.Location is not a location point we cannot get a position from it; so we use a bounding box approach.
					// Note that, as of Revit 2020, BasePoint has 2 new properties: Position for base point and SharedPosition for survey point.
					BoundingBoxXYZ BasePointBoundingBox = BasePointLocation.get_BoundingBox(CurrentDocument.ActiveView);
					if (BasePointBoundingBox == null)
					{
						continue;
					}

					// Create a new Datasmith placeholder actor for the base point.
					// Hash the Datasmith placeholder actor name to shorten it.
					string ActorName = BasePointLocation.IsShared ? "SurveyPoint" : "BasePoint";
					string ActorLabel = BasePointLocation.IsShared ? "Survey Point" : "Base Point";
					string HashedActorName = FDatasmithFacadeElement.GetStringHash(ActorName);
					FDatasmithFacadeActor BasePointActor = new FDatasmithFacadeActor(HashedActorName);
					BasePointActor.SetLabel(ActorLabel);

					// Set the world transform of the Datasmith placeholder actor.
					XYZ BasePointPosition = BasePointBoundingBox.Min;

					Transform TranslationMatrix = Transform.CreateTranslation(BasePointPosition);
					FDocumentData.SetActorTransform(TranslationMatrix.Multiply(InWorldTransform), BasePointActor);

					// Set the Datasmith placeholder actor layer to the base point category name.
					BasePointActor.SetLayer(BasePointLocation.Category.Name);

					// Add the Revit element ID and Unique ID tags to the Datasmith placeholder actor.
					BasePointActor.AddTag($"Revit.Element.Id.{BasePointLocation.Id.IntegerValue}");
					BasePointActor.AddTag($"Revit.Element.UniqueId.{BasePointLocation.UniqueId}");

					// Add a Revit element base point tag to the Datasmith placeholder actor.
					BasePointActor.AddTag("Revit.Element." + ActorName);

					// Add base point metadata to the Datasmith actor.
					string MetadataPrefix = BasePointLocation.IsShared ? "SurveyPointLocation*" : "BasePointLocation*";

					FDatasmithFacadeMetaData BasePointMetaData = new FDatasmithFacadeMetaData(BasePointActor.GetName() + "_DATA");
					BasePointMetaData.SetLabel(BasePointActor.GetLabel());
					BasePointMetaData.SetAssociatedElement(BasePointActor);

					BasePointMetaData.AddPropertyVector(MetadataPrefix + "Location", $"{BasePointPosition.X} {BasePointPosition.Y} {BasePointPosition.Z}");
					FDocumentData.AddActorMetadata(BasePointLocation, MetadataPrefix, BasePointMetaData);

					// Collect the base point placeholder actor into the Datasmith actor dictionary.
					FBaseElementData BasePointElement = new FBaseElementData(BasePointActor, BasePointMetaData, this);
					BasePointElement.bOptimizeHierarchy = false;
					CurrentExportData.ActorMap[BasePointLocation.Id] = BasePointElement;
				}
			}
		}

		private void CollectMesh(
			FDatasmithFacadeMesh InMesh
		)
		{
			if (InMesh.GetVertexCount() > 0 && InMesh.GetTriangleCount() > 0)
			{
				string MeshName = InMesh.GetName();

				if (!CurrentExportData.MeshMap.ContainsKey(MeshName))
				{
					// Keep track of the Datasmith mesh.
					CurrentExportData.MeshMap[MeshName] = InMesh;
				}
			}
		}

		private void AddCollectedMeshes(
			FDatasmithFacadeScene InDatasmithScene
		)
		{
			// Add the collected meshes from the Datasmith mesh dictionary to the Datasmith scene.
			foreach (FDatasmithFacadeMesh CollectedMesh in CurrentExportData.MeshMap.Values)
			{
				InDatasmithScene.AddMesh(CollectedMesh);
			}
		}

		private void AddHostHierarchy()
		{
			AddParentElementHierarchy(GetHostElement);
		}

		private void AddLevelHierarchy()
		{
			AddParentElementHierarchy(GetLevelElement);
		}

		private void AddCollectedMaterials(
			FDatasmithFacadeScene InDatasmithScene
		)
		{
			// Add the collected master materials from the material data dictionary to the Datasmith scene.
			foreach (FMaterialData CollectedMaterialData in CurrentExportData.MaterialDataMap.Values)
			{
				InDatasmithScene.AddMaterial(CollectedMaterialData.MasterMaterial);

				if (CollectedMaterialData.MessageList.Count > 0)
				{
					MessageList.AddRange(CollectedMaterialData.MessageList);
				}
			}
		}

		private Element GetHostElement(
			ElementId InElementId
		)
		{
			Element SourceElement = CurrentDocument.GetElement(InElementId);

			if (SourceElement as FamilyInstance != null)
			{
				return (SourceElement as FamilyInstance).Host;
			}
			else if (SourceElement as Wall != null)
			{
				return CurrentDocument.GetElement((SourceElement as Wall).StackedWallOwnerId);
			}
			else if (SourceElement as ContinuousRail != null)
			{
				return CurrentDocument.GetElement((SourceElement as ContinuousRail).HostRailingId);
			}
			else if (SourceElement.GetType().IsSubclassOf(typeof(InsulationLiningBase)))
			{
				return CurrentDocument.GetElement((SourceElement as InsulationLiningBase).HostElementId);
			}

			return null;
		}

		private Element GetLevelElement(
			ElementId InElementId
		)
		{
			Element SourceElement = CurrentDocument.GetElement(InElementId);

			return (SourceElement == null) ? null : CurrentDocument.GetElement(SourceElement.LevelId);
		}

		private void AddParentElementHierarchy(
			Func<ElementId, Element> InGetParentElement
		)
		{
			Queue<ElementId> ElementIdQueue = new Queue<ElementId>(CurrentExportData.ActorMap.Keys);

			// Make sure the Datasmith actor dictionary contains actors for all the Revit parent elements.
			while (ElementIdQueue.Count > 0)
			{
				Element ParentElement = InGetParentElement(ElementIdQueue.Dequeue());

				if (ParentElement == null)
				{
					continue;
				}

				ElementId ParentElementId = ParentElement.Id;

				if (CurrentExportData.ActorMap.ContainsKey(ParentElementId))
				{
					continue;
				}

				if (IsActorCached(ParentElement.Id))
				{
					// Move parent actor out of cache.
					CurrentExportData.ActorMap[ParentElementId] = CachedExportData.ActorMap[ParentElementId];
				}
				else
				{
					PushElement(ParentElement, Transform.Identity);
					PopElement();
				}

				ElementIdQueue.Enqueue(ParentElementId);
			}

			// Add the parented actors as children of the parent Datasmith actors.
			foreach (ElementId ElemId in new List<ElementId>(CurrentExportData.ActorMap.Keys))
			{
				Element ParentElement = InGetParentElement(ElemId);

				if (ParentElement == null)
				{
					continue;
				}

				Element SourceElement = CurrentDocument.GetElement(ElemId);

				if ((SourceElement as FamilyInstance != null && ParentElement as Truss != null) ||
					(SourceElement as Mullion != null) ||
					(SourceElement as Panel != null) ||
					(SourceElement as ContinuousRail != null))
				{
					// The Datasmith actor is a component in the hierarchy.
					CurrentExportData.ActorMap[ElemId].ElementActor.SetIsComponent(true);
				}

				ElementId ParentElementId = ParentElement.Id;

				// Add the parented actor as child of the parent Datasmith actor.
				CurrentExportData.ActorMap[ParentElementId].ChildElements.Add(CurrentExportData.ActorMap[ElemId]);

				// Prevent the parent Datasmith actor from being removed by optimization.
				CurrentExportData.ActorMap[ParentElementId].bOptimizeHierarchy = false;
			}

			// Remove the parented child actors from the Datasmith actor dictionary.
			foreach (ElementId ElemId in new List<ElementId>(CurrentExportData.ActorMap.Keys))
			{
				if (CachedExportData != null)
				{
					CachedExportData.ActorMap[ElemId] = CurrentExportData.ActorMap[ElemId];
				}

				Element ParentElement = InGetParentElement(ElemId);

				if (ParentElement == null)
				{
					continue;
				}

				// Remove the parented child actor from the Datasmith actor dictionary.
				CurrentExportData.ActorMap.Remove(ElemId);
			}
		}

		private static void SetActorTransform(
			Transform InWorldTransform,
			FDatasmithFacadeActor IOActor
		)
		{
			XYZ transformBasisX = InWorldTransform.BasisX;
			XYZ transformBasisY = InWorldTransform.BasisY;
			XYZ transformBasisZ = InWorldTransform.BasisZ;
			XYZ transformOrigin = InWorldTransform.Origin;

			float[] worldMatrix = new float[16];

			worldMatrix[0] = (float)transformBasisX.X;
			worldMatrix[1] = (float)transformBasisX.Y;
			worldMatrix[2] = (float)transformBasisX.Z;
			worldMatrix[3] = 0.0F;
			worldMatrix[4] = (float)transformBasisY.X;
			worldMatrix[5] = (float)transformBasisY.Y;
			worldMatrix[6] = (float)transformBasisY.Z;
			worldMatrix[7] = 0.0F;
			worldMatrix[8] = (float)transformBasisZ.X;
			worldMatrix[9] = (float)transformBasisZ.Y;
			worldMatrix[10] = (float)transformBasisZ.Z;
			worldMatrix[11] = 0.0F;
			worldMatrix[12] = (float)transformOrigin.X;
			worldMatrix[13] = (float)transformOrigin.Y;
			worldMatrix[14] = (float)transformOrigin.Z;
			worldMatrix[15] = 1.0F;

			// Set the world transform of the Datasmith actor.
			IOActor.SetWorldTransform(worldMatrix);
		}

		public static ElementType GetElementType(Element InElement)
		{
			return InElement.Document.GetElement(InElement.GetTypeId()) as ElementType;
		}

		public static string GetCategoryName(Element InElement)
		{
			ElementType Type = GetElementType(InElement);
			return Type?.Category?.Name ?? InElement.Category?.Name;
		}

		public static void AddActorMetadata(
			Element InElement,
			FDatasmithFacadeMetaData ActorMetadata
		)
		{
			// Add the Revit element category name metadata to the Datasmith actor.
			string CategoryName = GetCategoryName(InElement);
			if (!string.IsNullOrEmpty(CategoryName))
			{
				ActorMetadata.AddPropertyString("Element*Category", CategoryName);
			}

			// Add the Revit element family name metadata to the Datasmith actor.
			ElementType ElemType = GetElementType(InElement);
			string FamilyName = ElemType?.FamilyName;
			if (!string.IsNullOrEmpty(FamilyName))
			{
				ActorMetadata.AddPropertyString("Element*Family", FamilyName);
			}

			// Add the Revit element type name metadata to the Datasmith actor.
			string TypeName = ElemType?.Name;
			if (!string.IsNullOrEmpty(TypeName))
			{
				ActorMetadata.AddPropertyString("Element*Type", TypeName);
			}

			// Add Revit element metadata to the Datasmith actor.
			FDocumentData.AddActorMetadata(InElement, "Element*", ActorMetadata);

			if (ElemType != null)
			{
				// Add Revit element type metadata to the Datasmith actor.
				FDocumentData.AddActorMetadata(ElemType, "Type*", ActorMetadata);
			}
		}

		private static void AddActorMetadata(
			Element InSourceElement,
			string InMetadataPrefix,
			FDatasmithFacadeMetaData ElementMetaData
		)
		{
			IList<Parameter> Parameters = InSourceElement.GetOrderedParameters();

			if (Parameters != null)
			{
				foreach (Parameter Parameter in Parameters)
				{
					if (Parameter.HasValue)
					{
						string ParameterValue = Parameter.AsValueString();

						if (string.IsNullOrEmpty(ParameterValue))
						{
							switch (Parameter.StorageType)
							{
								case StorageType.Integer:
									ParameterValue = Parameter.AsInteger().ToString();
									break;
								case StorageType.Double:
									ParameterValue = Parameter.AsDouble().ToString();
									break;
								case StorageType.String:
									ParameterValue = Parameter.AsString();
									break;
								case StorageType.ElementId:
									ParameterValue = Parameter.AsElementId().ToString();
									break;
							}
						}

						if (!string.IsNullOrEmpty(ParameterValue))
						{
							string MetadataKey = InMetadataPrefix + Parameter.Definition.Name;
							ElementMetaData.AddPropertyString(MetadataKey, ParameterValue);
						}
					}
				}
			}
		}
	}
}
