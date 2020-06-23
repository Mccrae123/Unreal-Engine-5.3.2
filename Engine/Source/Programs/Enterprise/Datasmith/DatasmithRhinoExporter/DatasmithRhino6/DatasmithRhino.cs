// Copyright Epic Games, Inc. All Rights Reserved.

using Rhino;
using System;
using System.Collections.Generic;

namespace DatasmithRhino
{
	///<summary>
	/// <para>Every RhinoCommon .rhp assembly must have one and only one PlugIn-derived
	/// class. DO NOT create instances of this class yourself. It is the
	/// responsibility of Rhino to create an instance of this class.</para>
	/// <para>To complete plug-in information, please also see all PlugInDescription
	/// attributes in AssemblyInfo.cs (you might need to click "Project" ->
	/// "Show All Files" to see it in the "Solution Explorer" window).</para>
	///</summary>
	public class DatasmithRhino6 : Rhino.PlugIns.FileExportPlugIn
	{
		public DatasmithRhino6()
		{
			Instance = this;
		}

		///<summary>Gets the only instance of the DatasmithRhino6 plug-in.</summary>
		public static DatasmithRhino6 Instance {
			get; private set;
		}

		/// <summary>Defines file extensions that this export plug-in is designed to write.</summary>
		/// <param name="options">Options that specify how to write files.</param>
		/// <returns>A list of file types that can be exported.</returns>
		protected override Rhino.PlugIns.FileTypeList AddFileTypes(Rhino.FileIO.FileWriteOptions options)
		{
			var result = new Rhino.PlugIns.FileTypeList();
			result.AddFileType("Datasmith File (*.udatasmith)", "udatasmith");
			return result;
		}

		/// <summary>
		/// Is called when a user requests to export a ".udatasmith" file.
		/// It is actually up to this method to write the file itself.
		/// </summary>
		/// <param name="filename">The complete path to the new file.</param>
		/// <param name="index">The index of the file type as it had been specified by the AddFileTypes method.</param>
		/// <param name="doc">The document to be written.</param>
		/// <param name="options">Options that specify how to write file.</param>
		/// <returns>A value that defines success or a specific failure.</returns>
		protected override Rhino.PlugIns.WriteFileResult WriteFile(string filename, int index, RhinoDoc doc, Rhino.FileIO.FileWriteOptions options)
		{
			DatasmithRhinoSceneExporter Exporter = new DatasmithRhinoSceneExporter();
			bool bSuccess = Exporter.Export(filename, doc);

			return bSuccess ? Rhino.PlugIns.WriteFileResult.Success : Rhino.PlugIns.WriteFileResult.Failure;
		}
	}
}