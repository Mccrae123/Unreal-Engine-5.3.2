// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;

namespace Horde.Server.Server
{
	/// <summary>
	/// Server Info
	/// </summary>
	public class GetServerInfoResponse
	{
        /// <summary>
		/// Server version info
		/// </summary>
        public string ServerVersion { get; set; }

		/// <summary>
		/// The current agent version string
		/// </summary>
		public string? AgentVersion { get; set; }

        /// <summary>
        /// The operating system server is hosted on
        /// </summary>
        public string OsDescription { get; set; }

		/// <summary>
		/// Constructor
		/// </summary>
		public GetServerInfoResponse(string? agentVersion)
        {
            FileVersionInfo versionInfo = FileVersionInfo.GetVersionInfo(Assembly.GetExecutingAssembly().Location);		
			ServerVersion = versionInfo.ProductVersion ?? String.Empty;
			AgentVersion = agentVersion;
			OsDescription = RuntimeInformation.OSDescription;			
		}
	}

	/// <summary>
	/// Gets ports configured for this server
	/// </summary>
	public class GetPortsResponse
	{
		/// <summary>
		/// Port for HTTP communication
		/// </summary>
		public int? Http { get; set; }

		/// <summary>
		/// Port number for HTTPS communication
		/// </summary>
		public int? Https { get; set; }

		/// <summary>
		/// Port number for unencrpyted HTTPS communication
		/// </summary>
		public int? UnencryptedHttp2 { get; set; }
	}
}

