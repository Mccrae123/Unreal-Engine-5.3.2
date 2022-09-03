// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using EpicGames.Core;
using EpicGames.Perforce;
using Horde.Build.Server;
using Horde.Build.Users;
using Horde.Build.Utilities;
using Microsoft.Extensions.Caching.Memory;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using OpenTracing;
using OpenTracing.Util;

namespace Horde.Build.Perforce
{
	/// <summary>
	/// P4API implementation of the Perforce service
	/// </summary>
	class PerforceService : IPerforceService, IDisposable
	{
		sealed class PooledConnection : IDisposable
		{
			public IPerforceConnection Perforce { get; }
			public IPerforceSettings Settings => Perforce.Settings;
			public Credentials Credentials { get; }
			public ClientRecord? Client { get; }
			public DateTime ExpiresAt { get; }

			InfoRecord? _info;

			public PooledConnection(IPerforceConnection perforce, Credentials credentials, ClientRecord? client, DateTime expiresAt)
			{
				Perforce = perforce;
				Credentials = credentials;
				Client = client;
				ExpiresAt = expiresAt;
			}

			public void Dispose()
			{
				Perforce.Dispose();
			}

			public async ValueTask<InfoRecord> GetInfoAsync(CancellationToken cancellationToken)
			{
				_info ??= await Perforce.GetInfoAsync(InfoOptions.ShortOutput, cancellationToken);
				return _info;
			}
		}

		class PooledConnectionHandle : IPerforceConnection
		{
			readonly PerforceService _owner;
			PooledConnection _inner;

			public ClientRecord? Client => _inner.Client;
			public Credentials Credentials => _inner.Credentials;

			public PooledConnectionHandle(PerforceService owner, PooledConnection inner)
			{
				_owner = owner;
				_inner = inner;
			}

			/// <inheritdoc/>
			public void Dispose()
			{
				if (_inner != null)
				{
					_owner.ReleasePooledConnection(_inner);
					_inner = null!;
				}
			}

			/// <inheritdoc/>
			public IPerforceSettings Settings => _inner.Perforce.Settings;

			/// <inheritdoc/>
			public ILogger Logger => _inner.Perforce.Logger;

			/// <inheritdoc/>
			public IPerforceOutput Command(string command, IReadOnlyList<string> arguments, IReadOnlyList<string>? fileArguments, byte[]? inputData, string? promptResponse, bool interceptIo)
			{
				return _inner.Perforce.Command(command, arguments, fileArguments, inputData, promptResponse, interceptIo);
			}

			/// <inheritdoc/>
			public PerforceRecord CreateRecord(List<KeyValuePair<string, object>> fields)
			{
				return _inner.Perforce.CreateRecord(fields);
			}

			public ValueTask<InfoRecord> GetInfoAsync(CancellationToken cancellationToken) => _inner.GetInfoAsync(cancellationToken);

		}

		class Credentials
		{
			public string UserName { get; }
			public string? Password { get; }
			public DateTime? ExpiresAt { get; }

			public Credentials(string userName, string? password, DateTime? expiresAt)
			{
				UserName = userName;
				Password = password;
				ExpiresAt = expiresAt;
			}
		}

		readonly PerforceLoadBalancer _loadBalancer;
		readonly LazyCachedValue<Task<Globals>> _cachedGlobals;
		readonly ILogger _logger;

		// Useful overrides for local debugging with read-only data
		readonly string? _perforceServerOverride;
		readonly string? _perforceUserOverride;

		readonly ServerSettings _settings;
		readonly Dictionary<string, Dictionary<string, Credentials>> _userCredentialsByCluster = new Dictionary<string, Dictionary<string, Credentials>>(StringComparer.OrdinalIgnoreCase);
		readonly IUserCollection _userCollection;
		readonly MemoryCache _userCache = new MemoryCache(new MemoryCacheOptions { SizeLimit = 2000 });

		readonly List<PooledConnection> _pooledConnections = new List<PooledConnection>();

		/// <summary>
		/// Constructor
		/// </summary>
		public PerforceService(PerforceLoadBalancer loadBalancer, MongoService mongoService, IUserCollection userCollection, IOptions<ServerSettings> settings, ILogger<PerforceService> logger)
		{
			_loadBalancer = loadBalancer;
			_cachedGlobals = new LazyCachedValue<Task<Globals>>(() => mongoService.GetGlobalsAsync(), TimeSpan.FromSeconds(30.0));
			_userCollection = userCollection;
			_settings = settings.Value;
			_logger = logger;

			if(settings.Value.UseLocalPerforceEnv)
			{
				IPerforceSettings perforceSettings = PerforceSettings.Default;
				_perforceServerOverride = perforceSettings.ServerAndPort;
				_perforceUserOverride = perforceSettings.UserName;
			}
		}

		public void Dispose()
		{
			foreach (PooledConnection pooledConnection in _pooledConnections)
			{
				pooledConnection.Dispose();
			}
			_pooledConnections.Clear();

			_userCache.Dispose();
		}

		async Task<PooledConnectionHandle> CreatePooledConnectionAsync(string serverAndPort, Credentials credentials, ClientRecord? clientRecord, CancellationToken cancellationToken)
		{
			_ = cancellationToken;

			PerforceSettings settings = new PerforceSettings(serverAndPort, credentials.UserName);
			settings.AppName = "Horde.Build";
			settings.Password = credentials.Password;
			settings.ClientName = clientRecord?.Name ?? "__DOES_NOT_EXIST__";
			settings.PreferNativeClient = true;

			DateTime expiresAt = DateTime.UtcNow + TimeSpan.FromHours(1.0);
			if (credentials.ExpiresAt.HasValue && credentials.ExpiresAt.Value < expiresAt)
			{
				expiresAt = credentials.ExpiresAt.Value;
			}

			IPerforceConnection connection = await PerforceConnection.CreateAsync(settings, _logger);
			PooledConnection pooledConnection = new PooledConnection(connection, credentials, clientRecord, expiresAt);
			return new PooledConnectionHandle(this, pooledConnection);
		}

		PooledConnectionHandle? GetPooledConnection(Predicate<PooledConnection> predicate)
		{
			lock (_pooledConnections)
			{
				ReleaseExpiredConnections();
				for (int idx = 0; idx < _pooledConnections.Count; idx++)
				{
					PooledConnection connection = _pooledConnections[idx];
					if (predicate(connection))
					{
						_pooledConnections.RemoveAt(idx);
						return new PooledConnectionHandle(this, connection);
					}
				}
				return null;
			}
		}

		PooledConnectionHandle? GetPooledConnectionForUser(PerforceCluster cluster, string? userName)
		{
			string? poolUserName = userName ?? GetServiceUserCredentials(cluster).UserName;
			return GetPooledConnection(x => IsValidServer(x, cluster) && String.Equals(x.Settings.UserName, poolUserName, StringComparison.Ordinal) && x.Client == null);
		}

		PooledConnectionHandle? GetPooledConnectionForClient(PerforceCluster cluster, string? userName, string clientName)
		{
			string? poolUserName = userName ?? GetServiceUserCredentials(cluster).UserName;
			return GetPooledConnection(x => IsValidServer(x, cluster) && String.Equals(x.Settings.UserName, poolUserName, StringComparison.Ordinal) && String.Equals(x.Client?.Name, clientName, StringComparison.Ordinal));
		}

		PooledConnectionHandle? GetPooledConnectionForStream(PerforceCluster cluster, string? userName, string streamName)
		{
			string? poolUserName = userName ?? GetServiceUserCredentials(cluster).UserName;
			return GetPooledConnection(x => IsValidServer(x, cluster) && String.Equals(x.Settings.UserName, poolUserName, StringComparison.Ordinal) && x.Client != null && String.Equals(x.Client.Stream, streamName, StringComparison.Ordinal));
		}

		bool IsValidServer(PooledConnection connection, PerforceCluster cluster)
		{
			string serverAndPort = connection.Settings.ServerAndPort;
			if (_perforceServerOverride != null)
			{
				return String.Equals(serverAndPort, _perforceServerOverride, StringComparison.Ordinal);
			}
			else
			{
				return cluster.Servers.Any(x => x.ServerAndPort.Equals(serverAndPort, StringComparison.Ordinal));
			}
		}

		void ReleaseExpiredConnections()
		{
			DateTime utcNow = DateTime.UtcNow;
			for (int idx = _pooledConnections.Count - 1; idx >= 0; idx--)
			{
				PooledConnection connection = _pooledConnections[idx];
				if (connection.ExpiresAt < utcNow)
				{
					connection.Dispose();
					_pooledConnections.RemoveAt(idx);
				}
			}
		}

		void ReleasePooledConnection(PooledConnection connection)
		{
			lock (_pooledConnections)
			{
				ReleaseExpiredConnections();
				if (_pooledConnections.Count >= _settings.PerforceConnectionPoolSize)
				{
					_pooledConnections[0].Dispose();
					_pooledConnections.RemoveAt(0);
				}
				_pooledConnections.Add(connection);
			}
		}

		Credentials GetServiceUserCredentials(PerforceCluster cluster)
		{
			if (_perforceUserOverride != null)
			{
				return new Credentials(_perforceUserOverride, null, null);
			}
			else if (cluster.ServiceAccount != null)
			{
				PerforceCredentials? credentials = cluster.Credentials.FirstOrDefault(x => x.UserName.Equals(cluster.ServiceAccount, StringComparison.OrdinalIgnoreCase));
				if (credentials == null)
				{
					throw new Exception($"No credentials defined for {cluster.ServiceAccount} on {cluster.Name}");
				}
				return new Credentials(credentials.UserName, credentials.Password, null);
			}
			else
			{
				return new Credentials(PerforceSettings.Default.UserName, null, null);
			}
		}

		async Task<Credentials> GetCredentialsAsync(PerforceCluster cluster, string? userName, CancellationToken cancellationToken)
		{
			if (_perforceUserOverride != null)
			{
				return new Credentials(_perforceUserOverride, null, null);
			}
			else if (userName != null)
			{
				if (cluster.CanImpersonate && cluster.ServiceAccount != null)
				{
					return await GetTicketAsync(cluster, userName, cancellationToken);
				}
				else
				{
					return new Credentials(userName, null, null);
				}
			}
			else
			{
				return GetServiceUserCredentials(cluster);
			}
		}

		public async Task<IPerforceConnection> ConnectAsync(string clusterName, string? userName = null, CancellationToken cancellationToken = default)
		{
			PerforceCluster cluster = await GetClusterAsync(clusterName);
			return await ConnectAsync(cluster, userName, cancellationToken);
		}

		async Task<PooledConnectionHandle> ConnectAsync(PerforceCluster cluster, string? userName, CancellationToken cancellationToken)
		{
			PooledConnectionHandle? handle = GetPooledConnectionForUser(cluster, userName);
			if (handle == null)
			{
				IPerforceServer server = await GetServerAsync(cluster);
				Credentials credentials = await GetCredentialsAsync(cluster, userName, cancellationToken);
				handle = await CreatePooledConnectionAsync(server.ServerAndPort, credentials, null, cancellationToken);
			}
			return handle;
		}

		async Task<PooledConnectionHandle> ConnectAsChangeOwnerAsync(PerforceCluster cluster, int change, CancellationToken cancellationToken)
		{
			using (IPerforceConnection perforce = await ConnectAsync(cluster, null, cancellationToken))
			{
				// Get the change information
				ChangeRecord changeRecord = await perforce.GetChangeAsync(GetChangeOptions.None, change, cancellationToken);
				if (changeRecord.Client == null)
				{
					throw new PerforceException("Client not specified on changelist {Change}", changeRecord.Number);
				}
				if (changeRecord.User == null)
				{
					throw new PerforceException("User not specified on changelist {Change}", changeRecord.Number);
				}

				// Check if there's an existing connection for this client and user. We can forgo any server checks if the client matches, because we know it'll be on the same edge server.
				PooledConnectionHandle? handle = GetPooledConnectionForClient(cluster, changeRecord.User, changeRecord.Client);
				if (handle != null)
				{
					return handle;
				}

				// Get the client that owns this change
				ClientRecord clientRecord = await perforce.GetClientAsync(changeRecord.Client, cancellationToken);

				// Get the server address to connect as, if it's locked to an edge server
				string? serverAndPort = null;
				if (!String.IsNullOrEmpty(clientRecord.ServerId))
				{
					ServerRecord serverRecord = await perforce.GetServerAsync(clientRecord.ServerId, cancellationToken);
					if (!String.IsNullOrEmpty(serverRecord.Address))
					{
						serverAndPort = serverRecord.Address;
					}
				}

				// Otherwise connect to the default
				if (serverAndPort == null)
				{
					IPerforceServer server = await GetServerAsync(cluster);
					serverAndPort = server.ServerAndPort;
				}

				// Create a new connection
				Credentials credentials = await GetCredentialsAsync(cluster, changeRecord.User, cancellationToken);
				return await CreatePooledConnectionAsync(serverAndPort, credentials, clientRecord, cancellationToken);
			}
		}

		async Task<PooledConnectionHandle> ConnectWithStreamClientAsync(PerforceCluster cluster, string? userName, string streamName, CancellationToken cancellationToken)
		{
			PooledConnectionHandle? handle = GetPooledConnectionForStream(cluster, userName, streamName);
			if (handle != null)
			{
				return handle;
			}

			using (PooledConnectionHandle perforce = await ConnectAsync(cluster, userName, cancellationToken))
			{
				InfoRecord info = await perforce.GetInfoAsync(cancellationToken);
				string clientName = GetClientName(info.UserName, info.ServerId ?? "unknown", streamName, readOnly: true);

				string root = RuntimeInformation.IsOSPlatform(OSPlatform.Linux) ? $"/tmp/{clientName}/" : $"{Path.GetTempPath()}{clientName}\\";

				ClientRecord client = new ClientRecord(clientName, info.UserName ?? String.Empty, root);
				client.Stream = streamName;
				await perforce.CreateClientAsync(client, cancellationToken);

				return await CreatePooledConnectionAsync(perforce.Settings.ServerAndPort, perforce.Credentials, client, cancellationToken);
			}
		}

		async Task<Credentials> GetTicketAsync(PerforceCluster cluster, string userName, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.TryGetImpersonatedConnectionSettings").StartActive();
			scope.Span.SetTag("ClusterName", cluster.Name);
			scope.Span.SetTag("UserName", userName);

			Credentials? ticketInfo = null;

			Dictionary<string, Credentials>? userTickets;
			lock (_userCredentialsByCluster)
			{
				// Check if we have a ticket
				if (!_userCredentialsByCluster.TryGetValue(cluster.Name, out userTickets))
				{
					userTickets = new Dictionary<string, Credentials>(StringComparer.OrdinalIgnoreCase);
					_userCredentialsByCluster[cluster.Name] = userTickets;
				}
				if (userTickets.TryGetValue(userName, out ticketInfo))
				{
					// if the credential expires within the next 15 minutes, refresh
					if (DateTime.UtcNow > ticketInfo.ExpiresAt)
					{
						userTickets.Remove(userName);
						ticketInfo = null;
					}
				}
			}

			if (ticketInfo == null)
			{
				PerforceResponse<LoginRecord> response;
				using (IPerforceConnection connection = await ConnectAsync(cluster, null, cancellationToken: cancellationToken))
				{
					response = await connection.TryLoginAsync(LoginOptions.AllHosts | LoginOptions.PrintTicket, userName, null, null, cancellationToken);
				}
				if (!response.Succeeded || response.Data.Ticket == null)
				{
					throw new PerforceException($"Unable to get impersonation credentials for {userName} for cluster {cluster.Name}");
				}

				DateTime expiresAt = DateTime.UtcNow + new TimeSpan(response.Data.TicketExpiration * TimeSpan.TicksPerSecond) - TimeSpan.FromMinutes(15.0);
				ticketInfo = new Credentials(userName, response.Data.Ticket, expiresAt);

				lock (_userCredentialsByCluster)
				{
					userTickets[userName] = ticketInfo;
				}
			}

			return ticketInfo;
		}

		public async ValueTask<IUser> FindOrAddUserAsync(string clusterName, string userName, CancellationToken cancellationToken = default)
		{
			PerforceCluster cluster = await GetClusterAsync(clusterName);
			return await FindOrAddUserAsync(cluster, userName, cancellationToken);
		}

		public async ValueTask<IUser> FindOrAddUserAsync(PerforceCluster cluster, string userName, CancellationToken cancellationToken = default)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.FindOrAddUserAsync").StartActive();
			scope.Span.SetTag("ClusterName", cluster.Name);
			scope.Span.SetTag("UserName", userName);
			
			IUser? user;
			if (!_userCache.TryGetValue((cluster.Name, userName), out user))
			{
				user = await _userCollection.FindUserByLoginAsync(userName);
				if (user == null)
				{
					UserRecord? userRecord = null;
					using (IPerforceConnection perforce = await ConnectAsync(cluster, null, cancellationToken))
					{
						PerforceResponseList<UserRecord> responses = await perforce.TryGetUserAsync(userName, cancellationToken);
						if (responses.Count > 0 && responses[0].Succeeded)
						{
							userRecord = responses[0].Data;
						}
						else
						{
							_logger.LogWarning("Unable to find user {UserName} on cluster {ClusterName}", userName, cluster.Name);
						}
					}
					user = await _userCollection.FindOrAddUserByLoginAsync(userRecord?.UserName ?? userName, userRecord?.FullName, userRecord?.Email);
				}

				using (ICacheEntry entry = _userCache.CreateEntry((cluster.Name, userName)))
				{
					entry.SetValue(user);
					entry.SetSize(1);
					entry.SetAbsoluteExpiration(TimeSpan.FromDays(1.0));
				}
			}
			return user!;
		}

		async Task<PerforceCluster> GetClusterAsync(string? clusterName, string? serverAndPort = null)
		{
			Globals globals = await _cachedGlobals.GetCached();

			PerforceCluster? cluster = globals.FindPerforceCluster(clusterName, serverAndPort);
			if (cluster == null)
			{
				throw new Exception($"Unknown Perforce cluster '{clusterName}'");
			}

			return cluster;
		}

		async Task<IPerforceServer> GetServerAsync(PerforceCluster cluster)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.SelectServer").StartActive();
			scope.Span.SetTag("ClusterName", cluster.Name);

			IPerforceServer? server = await _loadBalancer.SelectServerAsync(cluster);
			if (server == null)
			{
				throw new Exception($"Unable to select server from '{cluster.Name}'");
			}
			return server;
		}

		static string GetClientName(string? serviceUserName, string serverId, string stream, bool readOnly)
		{
			string clientName = $"zzt-horde-p4bridge-{serverId}-{Dns.GetHostName()}-{serviceUserName ?? "default"}-{stream.Replace("/", "+", StringComparison.OrdinalIgnoreCase)}";

			if (!readOnly)
			{
				clientName += "-write";
			}

			return clientName;
		}

		static int GetSyncRevision(string path, FileAction headAction, int headRev)
		{
			switch (headAction)
			{
				case FileAction.None:
				case FileAction.Add:
				case FileAction.Branch:
				case FileAction.MoveAdd:
				case FileAction.Edit:
				case FileAction.Integrate:
					return headRev;
				case FileAction.Delete:
				case FileAction.MoveDelete:
				case FileAction.Purge:
					return -1;
				default:
					throw new Exception($"Unrecognized P4 file change type '{headAction}' for file {path}#{headRev}");
			}
		}

		static ChangeFile CreateChangeFile(string relativePath, DescribeFileRecord metaData)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.CreateChangeFile (FileMetaData)").StartActive();
			scope.Span.SetTag("RelativePath", relativePath);
			
			int revision = GetSyncRevision(metaData.DepotFile, metaData.Action, metaData.Revision);
			Md5Hash? digest = String.IsNullOrEmpty(metaData.Digest) ? (Md5Hash?)null : Md5Hash.Parse(metaData.Digest);
			return new ChangeFile(relativePath, metaData.DepotFile, revision, metaData.FileSize, digest, metaData.Type);
		}

		/// <inheritdoc/>
		public async Task<List<ChangeSummary>> GetChangesAsync(string clusterName, string streamName, int? minChange, int? maxChange, int maxResults, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.GetChangesAsync").StartActive();
			scope.Span.SetTag("ClusterName", clusterName);
			scope.Span.SetTag("MinChange", minChange ?? -1);
			scope.Span.SetTag("MaxChange", maxChange ?? -1);
			scope.Span.SetTag("MaxResults", maxResults);
			
			PerforceCluster cluster = await GetClusterAsync(clusterName);

			List<ChangeSummary> results = new List<ChangeSummary>();
			using (IPerforceConnection perforce = await ConnectWithStreamClientAsync(cluster, null, streamName, cancellationToken))
			{
				string filter = GetFilter($"//{perforce.Settings.ClientName}/...", minChange, maxChange);

				List<ChangesRecord> changes = await perforce.GetChangesAsync(ChangesOptions.IncludeTimes | ChangesOptions.LongOutput, maxResults, ChangeStatus.Submitted, filter, cancellationToken);

				foreach (ChangesRecord change in changes)
				{
					IUser user = await FindOrAddUserAsync(clusterName, change.User, cancellationToken);
					results.Add(new ChangeSummary(change.Number, user, change.Path ?? String.Empty, change.Description));
				}
			}
			return results;
		}

		/// <inheritdoc/>
		public async Task<ChangeDetails> GetChangeDetailsAsync(string clusterName, string streamName, int changeNumber, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.GetChangeDetailsAsync").StartActive();
			scope.Span.SetTag("ClusterName", clusterName);
			scope.Span.SetTag("StreamName", streamName);
			scope.Span.SetTag("ChangeNumber", changeNumber);
			
			PerforceCluster cluster = await GetClusterAsync(clusterName);

			using (PooledConnectionHandle perforce = await ConnectAsync(cluster, null, cancellationToken))
			{
				DescribeRecord record = await perforce.DescribeAsync(changeNumber, cancellationToken);

				InfoRecord info = await perforce.GetInfoAsync(cancellationToken);

				List<ChangeFile> files = new List<ChangeFile>();
				foreach (DescribeFileRecord describeFile in record.Files)
				{
					string? relativePath;
					if (TryGetStreamRelativePath(describeFile.DepotFile, streamName, out relativePath))
					{
						files.Add(CreateChangeFile(relativePath, describeFile));
					}
				}

				IUser user = await FindOrAddUserAsync(cluster, record.User, cancellationToken);
				DateTime timeUtc = new DateTime(record.Time.Ticks - info.TimeZoneOffsetSecs * TimeSpan.TicksPerSecond, DateTimeKind.Utc);
				return new ChangeDetails(changeNumber, user, record.Path ?? String.Empty, record.Description, files, timeUtc);
			}
		}

		/// <inheritdoc/>
		public async Task<(CheckShelfResult, string?)> CheckShelfAsync(string clusterName, string streamName, int changeNumber, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.CheckPreflightAsync").StartActive();
			scope.Span.SetTag("ClusterName", clusterName);
			scope.Span.SetTag("StreamName", streamName);
			scope.Span.SetTag("ChangeNumber", changeNumber);

			PerforceCluster cluster = await GetClusterAsync(clusterName);
			IPerforceServer server = await GetServerAsync(cluster);

			using (IPerforceConnection perforce = await ConnectAsync(cluster, null, cancellationToken))
			{
				PerforceResponse<DescribeRecord> response = await perforce.TryDescribeAsync(DescribeOptions.Shelved, -1, changeNumber, cancellationToken);
				if(response.Error != null)
				{
					if (response.Error.Generic == PerforceGenericCode.Empty)
					{
						return (CheckShelfResult.NoChange, null);
					}
					else
					{
						response.EnsureSuccess();
					}
				}

				DescribeRecord change = response.Data;
				if (change.Files.Count == 0)
				{
					return (CheckShelfResult.NoShelvedFiles, null);
				}

				StreamRecord stream = await perforce.GetStreamAsync(streamName, true, cancellationToken);
				PerforceViewMap viewMap = PerforceViewMap.Parse(stream.View);

				bool bHasMappedFile = false;
				bool bHasUnmappedFile = false;
				foreach (DescribeFileRecord shelvedFile in change.Files)
				{
					if (viewMap.TryMapFile(shelvedFile.DepotFile, StringComparison.OrdinalIgnoreCase, out string _))
					{
						bHasMappedFile = true;
					}
					else
					{
						bHasUnmappedFile = true;
					}
				}

				if (bHasUnmappedFile)
				{
					return (bHasMappedFile ? CheckShelfResult.MixedStream : CheckShelfResult.WrongStream, null);
				}

				return (CheckShelfResult.Ok, change.Description);
			}
		}

		/// <inheritdoc/>
		public async Task<List<ChangeDetails>> GetChangeDetailsAsync(string clusterName, string streamName, IReadOnlyList<int> changeNumbers, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.GetChangeDetailsAsync").StartActive();
			scope.Span.SetTag("ClusterName", clusterName);
			scope.Span.SetTag("StreamName", streamName);
			scope.Span.SetTag("ChangeNumbers.Count", changeNumbers.Count);
			
			PerforceCluster cluster = await GetClusterAsync(clusterName);

			List<ChangeDetails> results = new List<ChangeDetails>();
			using (PooledConnectionHandle perforce = await ConnectWithStreamClientAsync(cluster, null, streamName, cancellationToken))
			{
				InfoRecord info = await perforce.GetInfoAsync(cancellationToken);

				List<DescribeRecord> records = await perforce.DescribeAsync(DescribeOptions.Shelved, -1, changeNumbers.ToArray(), cancellationToken);
				foreach (DescribeRecord record in records)
				{
					List<ChangeFile> files = new List<ChangeFile>();
					foreach (DescribeFileRecord describeFile in record.Files)
					{
						string? relativePath;
						if (TryGetStreamRelativePath(describeFile.DepotFile, streamName, out relativePath))
						{
							files.Add(CreateChangeFile(relativePath, describeFile));
						}
					}

					IUser user = await FindOrAddUserAsync(cluster, record.User, cancellationToken);
					DateTime timeUtc = new DateTime(record.Time.Ticks - info.TimeZoneOffsetSecs * TimeSpan.TicksPerSecond, DateTimeKind.Utc);

					results.Add(new ChangeDetails(record.Number, user, record.Path ?? String.Empty, record.Description, files, timeUtc));
				}
			}
			return results;
		}

		/// <inheritdoc/>
		public Task<int> DuplicateShelvedChangeAsync(string clusterName, int shelvedChange, CancellationToken cancellationToken)
		{
			throw new NotImplementedException();
		}

		/// <inheritdoc/>
		public async Task DeleteShelvedChangeAsync(string clusterName, int shelvedChange, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.DeleteShelvedChangeAsync").StartActive();
			scope.Span.SetTag("ClusterName", clusterName);
			scope.Span.SetTag("ShelvedChange", shelvedChange);
			
			PerforceCluster cluster = await GetClusterAsync(clusterName);

			using (IPerforceConnection perforce = await ConnectAsChangeOwnerAsync(cluster, shelvedChange, cancellationToken))
			{
				await perforce.TryDeleteShelvedFilesAsync(shelvedChange, FileSpecList.Any, cancellationToken);
				await perforce.DeleteChangeAsync(DeleteChangeOptions.None, shelvedChange, cancellationToken);
			}
		}

		/// <inheritdoc/>
		public async Task UpdateChangelistDescription(string clusterName, int change, string description, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.UpdateChangelistDescription").StartActive();
			scope.Span.SetTag("ClusterName", clusterName);
			scope.Span.SetTag("Change", change);

			try
			{
				PerforceCluster cluster = await GetClusterAsync(clusterName);
				using (IPerforceConnection perforce = await ConnectAsChangeOwnerAsync(cluster, change, cancellationToken))
				{
					ChangeRecord record = await perforce.GetChangeAsync(GetChangeOptions.None, change, cancellationToken);
					record.Description = description;
					await perforce.UpdateChangeAsync(UpdateChangeOptions.None, record, cancellationToken);
				}
			}
			catch (Exception ex)
			{
				_logger.LogError(ex, "Unable to update Changelist for CL {Change} to {Description}, {Message}", change, description, ex.Message);
			}
		}

		static async Task ResetClientAsync(IPerforceConnection perforce, CancellationToken cancellationToken)
		{
			await perforce.RevertAsync(-1, null, RevertOptions.KeepWorkspaceFiles, FileSpecList.Any, cancellationToken);

			List<ChangesRecord> changes = await perforce.GetChangesAsync(ChangesOptions.None, perforce.Settings.ClientName, -1, ChangeStatus.Pending, null, FileSpecList.Any, cancellationToken);
			foreach(ChangesRecord change in changes)
			{
				await perforce.DeleteShelvedFilesAsync(change.Number, FileSpecList.Any, cancellationToken);
				await perforce.DeleteChangeAsync(DeleteChangeOptions.None, change.Number, cancellationToken);
			}
		}

		/// <inheritdoc/>
		public async Task<int> CreateNewChangeAsync(string clusterName, string streamName, string filePath, string description, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.CreateNewChangeAsync").StartActive();
			scope.Span.SetTag("ClusterName", clusterName);
			scope.Span.SetTag("StreamName", streamName);
			scope.Span.SetTag("FilePath", filePath);
			
			PerforceCluster cluster = await GetClusterAsync(clusterName);

			using (PooledConnectionHandle perforce = await ConnectWithStreamClientAsync(cluster, null, streamName, cancellationToken))
			{
				string workspaceFilePath = $"//{perforce.Client!.Name}/{filePath.TrimStart('/')}";
				string diskFilePath = $"{perforce.Client!.Root + filePath.TrimStart('/')}";

				int attempt = 1;
				const int MaxAttempts = 5;

				for(; ;)
				{
					await ResetClientAsync(perforce, cancellationToken);

					string? depotPath = null;
					try
					{
						List<FStatRecord> files = await perforce.FStatAsync(workspaceFilePath, cancellationToken).ToListAsync(cancellationToken);
						if (files.Count == 0)
						{
							// File does not exist, create it
							string? directoryName = Path.GetDirectoryName(diskFilePath);

							if (String.IsNullOrEmpty(directoryName))
							{
								throw new Exception($"Invalid directory name for local client file, disk file path: {diskFilePath}");
							}

							// Create the directory
							if (!Directory.Exists(directoryName))
							{
								Directory.CreateDirectory(directoryName);

								if (!Directory.Exists(directoryName))
								{
									throw new Exception($"Unable to create directrory: {directoryName}");
								}
							}

							// Create the file
							if (!File.Exists(diskFilePath))
							{
								using (FileStream fileStream = File.OpenWrite(diskFilePath))
								{
									fileStream.Close();
								}

								if (!File.Exists(diskFilePath))
								{
									throw new Exception($"Unable to create local change file: {diskFilePath}");
								}
							}

							List<AddRecord> addFiles = await perforce.AddAsync(-1, workspaceFilePath, cancellationToken);
							if (addFiles.Count != 1)
							{
								throw new Exception($"Unable to add local change file,  local: {diskFilePath} : workspace: {workspaceFilePath}");
							}

							depotPath = addFiles[0].DepotFile;
						}
						else
						{
							List<SyncRecord> syncResults = await perforce.SyncAsync(SyncOptions.Force, -1, workspaceFilePath, cancellationToken).ToListAsync(cancellationToken);
							if (syncResults == null || syncResults.Count != 1)
							{
								throw new Exception($"Unable to sync file, workspace: {workspaceFilePath}");
							}

							List<EditRecord> editResults = await perforce.EditAsync(-1, syncResults[0].DepotFile.ToString(), cancellationToken);
							if (editResults == null || editResults.Count != 1)
							{
								throw new Exception($"Unable to edit file, workspace: {workspaceFilePath}");
							}

							depotPath = editResults[0].DepotFile;
						}

						if (String.IsNullOrEmpty(depotPath))
						{
							throw new Exception($"Unable to get depot path for: {workspaceFilePath}");
						}

						// create a new change
						ChangeRecord change = new ChangeRecord();
						change.Description = description;
						change.Files.Add(depotPath);
						change = await perforce.CreateChangeAsync(change, cancellationToken);

						SubmitRecord submit = await perforce.SubmitAsync(change.Number, SubmitOptions.SubmitUnchanged, cancellationToken);
						return submit.ChangeNumber;
					}
					catch (Exception ex)
					{
						if (attempt < MaxAttempts)
						{
							_logger.LogWarning(ex, "Unable to submit new changelist (file: {File}, attempt: {Attempt}/{MaxAttempts}) - {Message}", depotPath, attempt, MaxAttempts, ex.Message);
						}
						else
						{
							throw new PerforceException($"Unable to submit new changelist for {filePath} after {MaxAttempts} attempts");
						}
					}
					finally
					{
						if (File.Exists(diskFilePath))
						{
							try
							{
								File.SetAttributes(diskFilePath, FileAttributes.Normal);
								File.Delete(diskFilePath);
							}
							catch (Exception ex2)
							{
								_logger.LogWarning(ex2, "Unable to delete temp file {File}", diskFilePath);
							}
						}
					}
				}
			}
		}

		/// <inheritdoc/>
		public async Task<(int? Change, string Message)> SubmitShelvedChangeAsync(string clusterName, int change, int originalChange, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.SubmitShelvedChangeAsync").StartActive();
			scope.Span.SetTag("ClusterName", clusterName);
			scope.Span.SetTag("Change", change);
			scope.Span.SetTag("OriginalChange", originalChange);

			PerforceCluster cluster = await GetClusterAsync(clusterName);
			IPerforceServer server = await GetServerAsync(cluster);

			List<DescribeRecord> records;
			using (IPerforceConnection perforce = await ConnectAsync(cluster, null, cancellationToken))
			{
				records = await perforce.DescribeAsync(DescribeOptions.Shelved, -1, new int[] { change, originalChange }, cancellationToken);
				if (records.Count != 2)
				{
					throw new PerforceException("Unexpected response count when examining changes to submit; expected 2, got {NumRecords}", records.Count);
				}

				if (records[0].Files.Count != records[1].Files.Count)
				{
					return (null, $"Mismatched number of shelved files for change {change} and original change: {originalChange}");
				}
				if (records[0].Files.Count == 0)
				{
					return (null, $"No shelved file for change {originalChange}");
				}

				List<DescribeFileRecord> files0 = records[0].Files.OrderBy(x => x.DepotFile, StringComparer.Ordinal).ToList();
				List<DescribeFileRecord> files1 = records[1].Files.OrderBy(x => x.DepotFile, StringComparer.Ordinal).ToList();
				foreach ((DescribeFileRecord file0, DescribeFileRecord file1) in Enumerable.Zip(files0, files1))
				{
					if (!String.Equals(file0.DepotFile, file1.DepotFile, StringComparison.Ordinal) || !String.Equals(file0.Digest, file1.Digest, StringComparison.Ordinal) || file0.Action != file1.Action)
					{
						_logger.LogInformation("Mismatch in shelved files between {File0} or {File1}", file0.DepotFile, file1.DepotFile);
						return (null, $"Shelved files have been modified.");
					}
				}
			}

			using (IPerforceConnection perforce = await ConnectAsync(cluster, records[0].User, cancellationToken))
			{
				try
				{
					SubmitRecord submit = await perforce.SubmitShelvedAsync(change, cancellationToken);
					return (submit.ChangeNumber, records[0].Description);
				}
				catch (Exception ex)
				{
					_logger.LogError(ex, "Unable to submit shelved change {Change}: {Message}", change, ex.Message);
					return (null, $"Submit command failed: {ex.Message}");
				}
			}
		}

		/// <inheritdoc/>
		public async Task<int> GetCodeChangeAsync(string clusterName, string streamName, int change, CancellationToken cancellationToken)
		{
			using IScope scope = GlobalTracer.Instance.BuildSpan("PerforceService.GetCodeChangeAsync").StartActive();
			scope.Span.SetTag("ClusterName", clusterName);
			scope.Span.SetTag("StreamName", streamName);
			scope.Span.SetTag("Change", change);
			
			int maxChange = change;
			for (; ; )
			{
				// Query for the changes before this point
				List<ChangeSummary> changes = await GetChangesAsync(clusterName, streamName, null, maxChange, 10, cancellationToken);
				_logger.LogInformation("Finding last code change in {Stream} before {MaxChange}: {NumResults}", streamName, maxChange, changes.Count);
				if (changes.Count == 0)
				{
					return 0;
				}

				// Get the details for them
				List<ChangeDetails> detailsList = await GetChangeDetailsAsync(clusterName, streamName, changes.ConvertAll(x => x.Number), cancellationToken);
				foreach (ChangeDetails details in detailsList.OrderByDescending(x => x.Number))
				{
					ChangeContentFlags contentFlags = details.GetContentFlags();
					_logger.LogInformation("Change {Change} = {Flags}", details.Number, contentFlags.ToString());
					if ((details.GetContentFlags() & ChangeContentFlags.ContainsCode) != 0)
					{
						return details.Number;
					}
				}

				// Loop round again, adjusting our maximum changelist number
				maxChange = changes.Min(x => x.Number) - 1;
			}
		}

		/// <summary>
		/// Gets the wildcard filter for a particular range of changes
		/// </summary>
		/// <param name="basePath">Base path to find files for</param>
		/// <param name="minChange">Minimum changelist number to query</param>
		/// <param name="maxChange">Maximum changelist number to query</param>
		/// <returns>Filter string</returns>
		public static string GetFilter(string basePath, int? minChange, int? maxChange)
		{
			StringBuilder filter = new StringBuilder(basePath);
			if (minChange != null && maxChange != null)
			{
				filter.Append(CultureInfo.InvariantCulture, $"@{minChange},{maxChange}");
			}
			else if (minChange != null)
			{
				filter.Append(CultureInfo.InvariantCulture, $"@>={minChange}");
			}
			else if (maxChange != null)
			{
				filter.Append(CultureInfo.InvariantCulture, $"@<={maxChange}");
			}
			return filter.ToString();
		}

		/// <summary>
		/// Gets a stream-relative path from a depot path
		/// </summary>
		/// <param name="depotFile">The depot file to check</param>
		/// <param name="streamName">Name of the stream</param>
		/// <param name="relativePath">Receives the relative path to the file</param>
		/// <returns>True if the stream-relative path was returned</returns>
		public static bool TryGetStreamRelativePath(string depotFile, string streamName, [NotNullWhen(true)] out string? relativePath)
		{
			if (depotFile.StartsWith(streamName, StringComparison.OrdinalIgnoreCase) && depotFile.Length > streamName.Length && depotFile[streamName.Length] == '/')
			{
				relativePath = depotFile.Substring(streamName.Length);
				return true;
			}

			Match match = Regex.Match(depotFile, "^//[^/]+/[^/]+(/.*)$");
			if (match.Success)
			{
				relativePath = match.Groups[1].Value;
				return true;
			}

			relativePath = null;
			return false;
		}
	}
}
