// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using EpicGames.Horde.Storage;
using Jupiter.Common;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Options;
using OpenTelemetry.Trace;
using Serilog;

namespace Jupiter.Implementation
{
    // ReSharper disable once ClassNeverInstantiated.Global
    public class LastAccessServiceReferences : IHostedService, IDisposable
    {
        private readonly ILastAccessCache<LastAccessRecord> _lastAccessCacheRecord;
        private readonly IReferencesStore _referencesStore;
        private readonly INamespacePolicyResolver _namespacePolicyResolver;
        private readonly Tracer _tracer;
        private readonly ILogger _logger = Log.ForContext<LastAccessServiceReferences>();
        private Timer? _timer;
        private readonly UnrealCloudDDCSettings _settings;
        
        public bool Running { get; private set; }

        public LastAccessServiceReferences(IOptionsMonitor<UnrealCloudDDCSettings> settings, ILastAccessCache<LastAccessRecord> lastAccessCache, IReferencesStore referencesStore, INamespacePolicyResolver namespacePolicyResolver, Tracer tracer)
        {
            _lastAccessCacheRecord = lastAccessCache;
            _referencesStore = referencesStore;
            _namespacePolicyResolver = namespacePolicyResolver;
            _tracer = tracer;
            _settings = settings.CurrentValue;
        }

        public Task StartAsync(CancellationToken cancellationToken)
        {
            _logger.Information("Last Access Aggregation service starting.");

            _timer = new Timer(OnUpdate, null, TimeSpan.Zero,
                period: TimeSpan.FromSeconds(_settings.LastAccessRollupFrequencySeconds));
            Running = true;

            return Task.CompletedTask;
        }

        public async Task StopAsync(CancellationToken cancellationToken)
        {
            _logger.Information("Last Access Aggregation service stopping.");

            _timer?.Change(Timeout.Infinite, 0);
            Running = false;

            // process the last records we have built up
            await ProcessLastAccessRecords();
        }

        private void OnUpdate(object? state)
        {
            try
            {
                // call results to make sure we join the task
                ProcessLastAccessRecords().Wait();
            }
            catch (Exception e)
            {
                _logger.Error(e, "Exception thrown while processing last access records");
            }
        }

        internal async Task<List<(LastAccessRecord, DateTime)>> ProcessLastAccessRecords()
        {
            _logger.Information("Running Last Access Aggregation for refs");
            List<(LastAccessRecord, DateTime)> records = await _lastAccessCacheRecord.GetLastAccessedRecords();
            foreach ((LastAccessRecord record, DateTime lastAccessTime) in records)
            {
                if (!ShouldTrackLastAccess(record.Namespace))
                {
                    continue;
                }

                using TelemetrySpan scope = _tracer.StartActiveSpan("lastAccess.update")
                    .SetAttribute("operation.name", "lastAccess.update")
                    .SetAttribute("resource.name", $"{record.Namespace}:{record.Bucket}.{record.Key}");
                _logger.Debug("Updating last access time to {LastAccessTime} for {Record}", lastAccessTime, record);
                await _referencesStore.UpdateLastAccessTime(record.Namespace, record.Bucket, record.Key, lastAccessTime);
                // delay 10ms between each record to distribute the load more evenly for the db
                await Task.Delay(10);
            }

            return records;
        }

        private bool ShouldTrackLastAccess(NamespaceId ns)
        {
            return _namespacePolicyResolver.GetPoliciesForNs(ns).LastAccessTracking;
        }

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        protected virtual void Dispose(bool disposing)
        {
            if (disposing)
            {
                _timer?.Dispose();

            }
        }
    }
}
