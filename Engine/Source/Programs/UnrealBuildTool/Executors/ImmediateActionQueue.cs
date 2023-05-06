// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Core;
using Microsoft.Extensions.Logging;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using UnrealBuildTool.Artifacts;

namespace UnrealBuildTool
{
	/// <summary>
	/// Results from a run action
	/// </summary>
	/// <param name="LogLines">Console log lines</param>
	/// <param name="ExitCode">Process return code.  Zero is assumed to be success and all other values an error.</param>
	/// <param name="ExecutionTime">Wall time</param>
	/// <param name="ProcessorTime">CPU time</param>
	/// <param name="AdditionalDescription">Additional description of action</param>
	record ExecuteResults(List<string> LogLines, int ExitCode, TimeSpan ExecutionTime, TimeSpan ProcessorTime, string? AdditionalDescription = null);

	/// <summary>
	/// Defines the phase of execution
	/// </summary>
	enum ActionPhase : byte
	{

		/// <summary>
		/// Check for already existing artifacts for the inputs
		/// </summary>
		ArtifactCheck,

		/// <summary>
		/// Compile the action
		/// </summary>
		Compile,
	}

	/// <summary>
	/// Defines the type of the runner.
	/// </summary>
	enum ImmediateActionQueueRunnerType : byte
	{

		/// <summary>
		/// Will be used to queue jobs as part of general requests
		/// </summary>
		Automatic,

		/// <summary>
		/// Will only be used for manual requests to queue jobs
		/// </summary>
		Manual,
	}


	/// <summary>
	/// Actions are assigned a runner when they need executing
	/// </summary>
	/// <param name="Type">Type of the runner</param>
	/// <param name="ActionPhase">The action phase that this step supports</param>
	/// <param name="RunAction">Action to be run queue running a task</param>
	/// <param name="UseActionWeights">If true, use the action weight as a secondary limit</param>
	/// <param name="MaxActions">Maximum number of action actions</param>
	/// <param name="MaxActionWeight">Maximum weight of actions</param>
	record ImmediateActionQueueRunner(ImmediateActionQueueRunnerType Type, ActionPhase ActionPhase, Func<LinkedAction, System.Action?> RunAction, bool UseActionWeights = false, int MaxActions = int.MaxValue, double MaxActionWeight = int.MaxValue)
	{
		/// <summary>
		/// Current number of active actions
		/// </summary>
		public int ActiveActions = 0;

		/// <summary>
		/// Current weight of actions
		/// </summary>
		public double ActiveActionWeight = 0;

		/// <summary>
		/// True if the current limits have not been reached.
		/// </summary>
		public bool IsUnderLimits => ActiveActions < MaxActions && (!UseActionWeights || ActiveActionWeight < MaxActionWeight);
	}

	/// <summary>
	/// Helper class to manage the action queue
	///
	/// Running the queue can be done with a mixture of automatic and manual runners. Runners are responsible for performing
	/// the work associated with an action. Automatic runners will have actions automatically queued to them up to the point
	/// that any runtime limits aren't exceeded (such as maximum number of concurrent processes).  Manual runners must have
	/// jobs queued to them by calling TryStartOneAction or StartManyActions with the runner specified.
	/// 
	/// For example:
	/// 
	///		ParallelExecutor uses an automatic runner exclusively.
	///		BoxExecutor uses an automatic runner to run jobs locally and a manual runner to run jobs remotely as processes 
	///			become available.
	/// </summary>
	class ImmediateActionQueue : IDisposable
	{

		/// <summary>
		/// Running status of the action
		/// </summary>
		private enum ActionStatus : byte
		{
			/// <summary>
			/// Queued waiting for execution
			/// </summary>
			Queued,

			/// <summary>
			/// Action is running
			/// </summary>
			Running,

			/// <summary>
			/// Action has successfully finished
			/// </summary>
			Finished,

			/// <summary>
			/// Action has finished with an error
			/// </summary>
			Error,
		}

		/// <summary>
		/// Used to track the state of each action
		/// </summary>
		private struct ActionState
		{
			/// <summary>
			/// Action to be executed
			/// </summary>
			public LinkedAction Action;

			/// <summary>
			/// Phase of the action
			/// </summary>
			public ActionPhase Phase;

			/// <summary>
			/// Current status of the execution
			/// </summary>
			public ActionStatus Status;

			/// <summary>
			/// Runner assigned to the action
			/// </summary>
			public ImmediateActionQueueRunner? Runner;

			/// <summary>
			/// Optional execution results
			/// </summary>
			public ExecuteResults? Results;
		};

		/// <summary>
		/// State information about each action
		/// </summary>
		private readonly ActionState[] Actions;

		private readonly ReaderWriterLockSlim ActionsLock;

		/// <summary>
		/// Output logging
		/// </summary>
		public readonly ILogger Logger;

		/// <summary>
		/// Total number of actions
		/// </summary>
		public int TotalActions => Actions.Length;

		/// <summary>
		/// Process group
		/// </summary>		
		public readonly ManagedProcessGroup ProcessGroup = new();

		/// <summary>
		/// Source for the cancellation token
		/// </summary>
		public readonly CancellationTokenSource CancellationTokenSource = new();

		/// <summary>
		/// Cancellation token
		/// </summary>
		public CancellationToken CancellationToken => CancellationTokenSource.Token;

		/// <summary>
		/// Progress writer
		/// </summary>
		public readonly ProgressWriter ProgressWriter;

		/// <summary>
		/// Overall success of the action queue
		/// </summary>
		public bool Success = true;

		/// <summary>
		/// Whether to show compilation times along with worst offenders or not.
		/// </summary>
		public bool ShowCompilationTimes = false;

		/// <summary>
		/// Whether to show CPU utilization after the work is complete.
		/// </summary>
		public bool ShowCPUUtilization = false;

		/// <summary>
		/// Add target names for each action executed
		/// </summary>
		public bool PrintActionTargetNames = false;

		/// <summary>
		/// Whether to log command lines for actions being executed
		/// </summary>
		public bool LogActionCommandLines = false;

		/// <summary>
		/// Whether to show compilation times for each executed action
		/// </summary>
		public bool ShowPerActionCompilationTimes = false;

		/// <summary>
		/// Collapse non-error output lines
		/// </summary>
		public bool CompactOutput = false;

		/// <summary>
		/// When enabled, will stop compiling targets after a compile error occurs.
		/// </summary>
		public bool StopCompilationAfterErrors = false;

		/// <summary>
		/// Return true if the queue is done
		/// </summary>
		public bool IsDone => _doneTaskSource.Task.IsCompleted;

		/// <summary>
		/// Collection of available runners
		/// </summary>
		private readonly List<ImmediateActionQueueRunner> _runners = new();

		/// <summary>
		/// First action to start scanning for actions to run
		/// </summary>
		private int _firstPendingAction;

		/// <summary>
		/// Action to invoke when writing tool output
		/// </summary>
		private readonly Action<string> _writeToolOutput;

		/// <summary>
		/// Timer used to collect CPU utilization
		/// </summary>
		private Timer? _cpuUtilizationTimer;

		/// <summary>
		/// Per-second logging of cpu utilization
		/// </summary>
		private List<float> _cpuUtilization = new();

		/// <summary>
		/// Collection of all actions remaining to be logged
		/// </summary>
		private readonly List<int> _actionsToLog = new();

		/// <summary>
		/// Task waiting to process logging
		/// </summary>
		private Task? _actionsToLogTask = null;

		/// <summary>
		/// Used only by the logger to track the [x,total] output
		/// </summary>
		private int _loggedCompletedActions = 0;

		/// <summary>
		/// Tracks the number of completed actions.
		/// </summary>
		private int _completedActions = 0;

		/// <summary>
		/// Used to terminate the run with status
		/// </summary>
		private readonly TaskCompletionSource _doneTaskSource = new();

		/// <summary>
		/// If set, artifact cache used to retrieve previously compiled results and save new results
		/// </summary>
		private IActionArtifactCache? _actionArtifactCache;

		static ExecuteResults s_copiedFromCacheResults = new(new List<string>(), 0, TimeSpan.Zero, TimeSpan.Zero, "copied from cache");

		/// <summary>
		/// Construct a new instance of the action queue
		/// </summary>
		/// <param name="actions">Collection of actions</param>
		/// <param name="actionArtifactCache">If true, the artifact cache is being used</param>
		/// <param name="maxActionArtifactCacheTasks">Max number of concurrent artifact cache tasks</param>
		/// <param name="progressWriterText">Text to be displayed with the progress writer</param>
		/// <param name="writeToolOutput">Action to invoke when writing tool output</param>
		/// <param name="logger">Logging interface</param>
		public ImmediateActionQueue(IEnumerable<LinkedAction> actions, IActionArtifactCache? actionArtifactCache, int maxActionArtifactCacheTasks, string progressWriterText, Action<string> writeToolOutput, ILogger logger)
		{
			int count = actions.Count();
			Actions = new ActionState[count];
			ActionsLock = new ReaderWriterLockSlim();

			Logger = logger;
			ProgressWriter = new(progressWriterText, false, logger);
			_actionArtifactCache = actionArtifactCache;
			_writeToolOutput = writeToolOutput;

			bool readArtifacts = _actionArtifactCache != null && _actionArtifactCache.EnableReads;
			ActionPhase initialPhase = readArtifacts ? ActionPhase.ArtifactCheck : ActionPhase.Compile;
			int index = 0;
			foreach (LinkedAction action in actions)
			{
				action.SortIndex = index;
				Actions[index++] = new ActionState { Action = action, Status = ActionStatus.Queued, Phase = initialPhase, Results = null };
			}

			if (readArtifacts)
			{
				var runAction = (LinkedAction action) =>
				{
					return new System.Action(async () =>
					{
						bool success = await _actionArtifactCache!.CompleteActionFromCacheAsync(action, CancellationToken);
						if (success)
						{
							OnActionCompleted(action, success, s_copiedFromCacheResults);
						}
						else
						{
							RequeueAction(action);
						}
					});
				};

				_runners.Add(new(ImmediateActionQueueRunnerType.Automatic, ActionPhase.ArtifactCheck, runAction, false, maxActionArtifactCacheTasks, 0));
			}
		}

		/// <summary>
		/// Create a new automatic runner
		/// </summary>
		/// <param name="runAction">Action to be run queue running a task</param>
		/// <param name="useActionWeights">If true, use the action weight as a secondary limit</param>
		/// <param name="maxActions">Maximum number of action actions</param>
		/// <param name="maxActionWeight">Maximum weight of actions</param>
		/// <returns>Created runner</returns>
		public ImmediateActionQueueRunner CreateAutomaticRunner(Func<LinkedAction, System.Action?> runAction, bool useActionWeights, int maxActions, double maxActionWeight)
		{
			ImmediateActionQueueRunner runner = new(ImmediateActionQueueRunnerType.Automatic, ActionPhase.Compile, runAction, useActionWeights, maxActions, maxActionWeight);
			_runners.Add(runner);
			return runner;
		}

		/// <summary>
		/// Create a manual runner
		/// </summary>
		/// <param name="runAction">Action to be run queue running a task</param>
		/// <param name="useActionWeights">If true, use the action weight as a secondary limit</param>
		/// <param name="maxActions">Maximum number of action actions</param>
		/// <param name="maxActionWeight">Maximum weight of actions</param>
		/// <returns>Created runner</returns>
		public ImmediateActionQueueRunner CreateManualRunner(Func<LinkedAction, System.Action?> runAction, bool useActionWeights = false, int maxActions = int.MaxValue, double maxActionWeight = double.MaxValue)
		{
			ImmediateActionQueueRunner runner = new(ImmediateActionQueueRunnerType.Manual, ActionPhase.Compile, runAction, useActionWeights, maxActions, maxActionWeight);
			_runners.Add(runner);
			return runner;
		}

		/// <summary>
		/// Start the process of running all the actions
		/// </summary>
		public void Start()
		{
			if (ShowCPUUtilization)
			{
				_cpuUtilizationTimer = new(x =>
				{
					lock (_cpuUtilization)
					{
						if (Utils.GetTotalCpuUtilization(out float cpuUtilization))
						{
							_cpuUtilization.Add(cpuUtilization);
						}
					}
				}, null, 1000, 1000);
			}

			Logger.LogInformation("------ Building {TotalActions} action(s) started ------", TotalActions);
		}

		/// <summary>
		/// Run the actions until complete
		/// </summary>
		/// <returns>True if completed successfully</returns>
		public async Task<bool> RunTillDone()
		{
			await _doneTaskSource.Task;
			TraceSummary();
			return Success;
		}

		/// <summary>
		/// Return an enumeration of ready compile tasks.  This is not executed under a lock and 
		/// does not modify the state of any actions.
		/// </summary>
		/// <returns>Enumerations of all ready to compile actions.</returns>
		public IEnumerable<LinkedAction> EnumerateReadyToCompileActions()
		{
			foreach (ActionState actionState in Actions)
			{
				if (actionState.Status == ActionStatus.Queued && 
					actionState.Phase == ActionPhase.Compile && 
					GetActionReadyState(actionState.Action) == ActionReadyState.Ready)
				{
					yield return actionState.Action;
				}				
			}
		}

		public struct WriteLockScope : IDisposable
		{
			public WriteLockScope(ReaderWriterLockSlim lockObject)
			{
				_lockObject = lockObject;
				_lockObject.EnterWriteLock();
			}

			public void Exit()
			{
				if (!_inside)
					return;
				_lockObject.ExitWriteLock();
				_inside = false;
			}

			public void Dispose()
			{
				Exit();
			}

			ReaderWriterLockSlim _lockObject;
			bool _inside = true;
		}

		/// <summary>
		/// Try to start one action
		/// </summary>
		/// <param name="runner">If specified, tasks will only be queued to this runner.  Otherwise all manual runners will be used.</param>
		/// <returns>True if an action was run, false if not</returns>
		public bool TryStartOneAction(ImmediateActionQueueRunner? runner = null)
		{
			using (var actionsLock = new WriteLockScope(ActionsLock))
			{
				// If we are starting deeper in the action collection, then never set the first pending action location
				bool foundFirstPending = false;

				// Loop through the items
				for (int actionIndex = _firstPendingAction; actionIndex != Actions.Length; ++actionIndex)
				{

					// If the action has already reached the compilation state, then just ignore
					if (Actions[actionIndex].Status != ActionStatus.Queued)
					{
						continue;
					}

					// If needed, update the first valid slot for searching for actions to run
					if (!foundFirstPending)
					{
						_firstPendingAction = actionIndex;
						foundFirstPending = true;
					}

					// Based on the ready state, use this action or mark as an error
					switch (GetActionReadyState(Actions[actionIndex].Action))
					{
						case ActionReadyState.NotReady:
							break;

						case ActionReadyState.Error:
							Actions[actionIndex].Status = ActionStatus.Error;
							actionsLock.Exit();
							IncrementCompletedActions();
							break;

						case ActionReadyState.Ready:
							ImmediateActionQueueRunner? selectedRunner = null;
							System.Action? action = null;
							if (runner != null)
							{
								if (runner.IsUnderLimits && runner.ActionPhase == Actions[actionIndex].Phase)
								{
									action = runner.RunAction(Actions[actionIndex].Action);
									if (action != null)
									{
										selectedRunner = runner;
									}
								}
							}
							else
							{
								foreach (ImmediateActionQueueRunner tryRunner in _runners)
								{
									if (tryRunner.Type == ImmediateActionQueueRunnerType.Automatic &&                                   
										tryRunner.IsUnderLimits && tryRunner.ActionPhase == Actions[actionIndex].Phase)
									{
										action = tryRunner.RunAction(Actions[actionIndex].Action);
										if (action != null)
										{
											selectedRunner = tryRunner;
											break;
										}
									}
								}
							}
							
							if (action != null && selectedRunner != null)
							{
								Actions[actionIndex].Status = ActionStatus.Running;
								Actions[actionIndex].Runner = selectedRunner;
								selectedRunner.ActiveActions++;
								selectedRunner.ActiveActionWeight += Actions[actionIndex].Action.Weight;
								actionsLock.Exit();
								Task.Factory.StartNew(() => action(), CancellationToken, TaskCreationOptions.LongRunning | TaskCreationOptions.PreferFairness, TaskScheduler.Default);
								return true;
							}
							break;

						default:
							throw new ApplicationException("Unexpected action ready state");
					}
				}

				// We found nothing, check to see if we have no active running tasks and no manual runners.
				// If we don't, then it is assumed we can't queue any more work.
				bool prematureDone = true;
				foreach (ImmediateActionQueueRunner tryRunner in _runners)
				{
					if (tryRunner.Type == ImmediateActionQueueRunnerType.Manual || tryRunner.ActiveActions != 0)
					{
						prematureDone = false;
						break;
					}
				}
				if (prematureDone && !_doneTaskSource.Task.IsCompleted)
				{
					_doneTaskSource.SetResult();
				}
				return false;
			}
		}

		/// <summary>
		/// Start as many actions as possible.
		/// </summary>
		/// <param name="runner">If specified, all actions will be limited to the runner</param>
		public void StartManyActions(ImmediateActionQueueRunner? runner = null)
		{
			while (TryStartOneAction(runner))
			{ }
		}

		/// <summary>
		/// Dispose the object
		/// </summary>
		public void Dispose()
		{
			if (_cpuUtilizationTimer != null)
			{
				_cpuUtilizationTimer.Dispose();
			}
			ProcessGroup.Dispose();
		}

		/// <summary>
		/// Reset the status of the given action to the queued state.
		/// </summary>
		/// <param name="action">Action being re-queued</param>
		public void RequeueAction(LinkedAction action)
		{
			SetActionState(action, ActionStatus.Queued, null);
		}

		/// <summary>
		/// Notify the system that an action has been completed.
		/// </summary>
		/// <param name="action">Action being completed</param>
		/// <param name="success">If true, the action succeeded or false if it failed.</param>
		/// <param name="results">Optional execution results</param>
		public void OnActionCompleted(LinkedAction action, bool success, ExecuteResults? results)
		{
			SetActionState(action, success ? ActionStatus.Finished : ActionStatus.Error, results);
		}

		/// <summary>
		/// Set the new state of an action.  Can only set state to Queued, Finished, or Error
		/// </summary>
		/// <param name="action">Action being set</param>
		/// <param name="status">Status to set</param>
		/// <param name="results">Optional results</param>
		/// <exception cref="BuildException"></exception>
		private void SetActionState(LinkedAction action, ActionStatus status, ExecuteResults? results)
		{
			int actionIndex = action.SortIndex;

			// Update the actions data
			using (var actionsLock = new WriteLockScope(ActionsLock))
			{
				ImmediateActionQueueRunner runner = Actions[actionIndex].Runner ?? throw new BuildException("Attempting to update action state but runner isn't set");
				runner.ActiveActions--;
				runner.ActiveActionWeight -= Actions[actionIndex].Action.Weight;

				// If we are doing an artifact check, then move to compile phase
				if (Actions[actionIndex].Phase == ActionPhase.ArtifactCheck)
				{
					Actions[actionIndex].Phase = ActionPhase.Compile;
					if (status != ActionStatus.Finished)
					{
						status = ActionStatus.Queued;
					}
				}

				Actions[actionIndex].Status = status;
				Actions[actionIndex].Runner = null;
				if (results != null)
				{
					Actions[actionIndex].Results = results;
				}

				// Add the action to the logging queue
				if (status != ActionStatus.Queued)
				{
					lock (_actionsToLog)
					{
						_actionsToLog.Add(action.SortIndex);
						if (_actionsToLogTask == null)
						{
							_actionsToLogTask = Task.Run(LogActions);
						}
					}
				}

				switch (status)
				{
					case ActionStatus.Queued:
						if (actionIndex < _firstPendingAction)
						{
							_firstPendingAction = actionIndex;
						}
						break;

					case ActionStatus.Finished:
						// Notify the artifact handler of the action completing.  We don't wait on the resulting task.  The
						// cache is required to remember any pending saves and a final call to Flush will wait for everything to complete.
						_actionArtifactCache?.ActionCompleteAsync(action, CancellationToken);
						actionsLock.Exit();
						IncrementCompletedActions();
						break;

					case ActionStatus.Error:
						Success = false;
						actionsLock.Exit();
						IncrementCompletedActions();
						break;

					default:
						throw new BuildException("Unexpected action status set");
				}
			}

			// Since something has been completed or returned to the queue, try to run actions again
			StartManyActions();
		}

		/// <summary>
		/// Increment the number of completed actions and signal done if all actions complete.
		/// This must be executed under the lock of Actions.
		/// </summary>
		private void IncrementCompletedActions()
		{
			var completedActions = Interlocked.Increment(ref _completedActions);
			if (completedActions >= Actions.Length && !_doneTaskSource.Task.IsCompleted)
			{
				_doneTaskSource.SetResult();
			}
		}

		/// <summary>
		/// Purge the pending logging actions
		/// </summary>
		private void LogActions()
		{
			for (; ; )
			{
				int[]? actionsToLog = null;
				lock (_actionsToLog)
				{
					if (_actionsToLog.Count == 0)
					{
						_actionsToLogTask = null;
					}
					else
					{
						actionsToLog = _actionsToLog.ToArray();
						_actionsToLog.Clear();
					}
				}

				if (actionsToLog == null)
				{
					return;
				}

				foreach (int index in actionsToLog)
				{
					LogAction(Actions[index].Action, Actions[index].Results);
				}
			}
		}

		private static int s_previousLineLength = -1;

		/// <summary>
		/// Log an action that has completed
		/// </summary>
		/// <param name="action">Action that has completed</param>
		/// <param name="executeTaskResult">Results of the action</param>
		private void LogAction(LinkedAction action, ExecuteResults? executeTaskResult)
		{
			List<string>? logLines = null;
			int exitCode = int.MaxValue;
			TimeSpan executionTime = TimeSpan.Zero;
			TimeSpan processorTime = TimeSpan.Zero;
			string? additionalDescription = null;
			if (executeTaskResult != null)
			{
				logLines = executeTaskResult.LogLines;
				exitCode = executeTaskResult.ExitCode;
				executionTime = executeTaskResult.ExecutionTime;
				processorTime = executeTaskResult.ProcessorTime;
				additionalDescription = executeTaskResult.AdditionalDescription;
			}

			// Write it to the log
			string description = string.Empty;
			if (action.bShouldOutputStatusDescription || (logLines != null && logLines.Count == 0))
			{
				description = $"{(action.CommandDescription ?? action.CommandPath.GetFileNameWithoutExtension())} {action.StatusDescription}".Trim();
			}
			else if (logLines != null && logLines.Count > 0)
			{
				description = $"{(action.CommandDescription ?? action.CommandPath.GetFileNameWithoutExtension())} {logLines[0]}".Trim();
			}
			if (!string.IsNullOrEmpty(additionalDescription))
			{
				description = $"{description} {additionalDescription}";
			}

			lock (ProgressWriter)
			{
				int totalActions = Actions.Length;
				int completedActions = Interlocked.Increment(ref _loggedCompletedActions);
				ProgressWriter.Write(completedActions, Actions.Length);

				// Canceled
				if (exitCode == int.MaxValue)
				{
					Logger.LogInformation("[{CompletedActions}/{TotalActions}] {Description} canceled", completedActions, totalActions, description);
					return;
				}

				string targetDetails = "";
				TargetDescriptor? target = action.Target;
				if (PrintActionTargetNames && target != null)
				{
					targetDetails = $"[{target.Name} {target.Platform} {target.Configuration}]";
				}

				if (LogActionCommandLines)
				{
					Logger.LogDebug("[{CompletedActions}/{TotalActions}]{TargetDetails} Command: {CommandPath} {CommandArguments}", completedActions, totalActions, targetDetails, action.CommandPath, action.CommandArguments);
				}

				string compilationTimes = "";

				if (ShowPerActionCompilationTimes)
				{
					if (processorTime.Ticks > 0)
					{
						compilationTimes = $" (Wall: {executionTime.TotalSeconds:0.00}s CPU: {processorTime.TotalSeconds:0.00}s)";
					}
					else
					{
						compilationTimes = $" (Wall: {executionTime.TotalSeconds:0.00}s)";
					}
				}

				string message = ($"[{completedActions}/{totalActions}]{targetDetails}{compilationTimes} {description}");

				if (CompactOutput)
				{
					if (s_previousLineLength > 0)
					{
						// move the cursor to the far left position, one line back
						Console.CursorLeft = 0;
						Console.CursorTop -= 1;
						// clear the line
						Console.Write("".PadRight(s_previousLineLength));
						// move the cursor back to the left, so output is written to the desired location
						Console.CursorLeft = 0;
					}
				}

				s_previousLineLength = message.Length;

				_writeToolOutput(message);
				if (logLines != null)
				{
					foreach (string Line in logLines.Skip(action.bShouldOutputStatusDescription ? 0 : 1))
					{
						// suppress library creation messages when writing compact output
						if (CompactOutput && Line.StartsWith("   Creating library ") && Line.EndsWith(".exp"))
						{
							continue;
						}

						_writeToolOutput(Line);

						// Prevent overwriting of logged lines
						s_previousLineLength = -1;
					}
				}

				if (exitCode != 0)
				{
					// BEGIN TEMPORARY TO CATCH PVS-STUDIO ISSUES
					if (logLines == null || logLines.Count == 0)
					{
						Logger.LogError("{TargetDetails} {Description}: Exited with error code {ExitCode}", targetDetails, description, exitCode);
						Logger.LogInformation("{TargetDetails} {Description}: WorkingDirectory {WorkingDirectory}", targetDetails, description, action.WorkingDirectory);
						Logger.LogInformation("{TargetDetails} {Description}: {CommandPath} {CommandArguments}", targetDetails, description, action.CommandPath, action.CommandArguments);
					}
					// END TEMPORARY

					// prevent overwriting of error text
					s_previousLineLength = -1;

					// Cancel all other pending tasks
					if (StopCompilationAfterErrors)
					{
						CancellationTokenSource.Cancel();
						if (!_doneTaskSource.Task.IsCompleted)
						{
							_doneTaskSource.SetResult();
						}
					}
				}
			}
		}

		/// <summary>
		/// Generate the final summary display
		/// </summary>
		private void TraceSummary()
		{

			// Wait for logging to complete
			Task? loggingTask = null;
			lock (_actionsToLog)
			{
				loggingTask = _actionsToLogTask;
			}
			loggingTask?.Wait();

			if (ShowCPUUtilization)
			{
				lock (_cpuUtilization)
				{
					if (_cpuUtilization.Count > 0)
					{
						Logger.LogInformation("");
						Logger.LogInformation("Average CPU Utilization: {CPUPercentage}%", (int)(_cpuUtilization.Average()));
					}
				}
			}

			if (!ShowCompilationTimes)
			{
				return;
			}

			Logger.LogInformation("");
			if (ProcessGroup.TotalProcessorTime.Ticks > 0)
			{
				Logger.LogInformation("Total CPU Time: {TotalSeconds} s", ProcessGroup.TotalProcessorTime.TotalSeconds);
				Logger.LogInformation("");
			}

			IEnumerable<int> CompletedActions = Enumerable.Range(0, Actions.Length)
				.Where(x => Actions[x].Results != null && Actions[x].Results!.ExecutionTime > TimeSpan.Zero)
				.OrderByDescending(x => Actions[x].Results!.ExecutionTime)
				.Take(20);

			if (CompletedActions.Any())
			{
				Logger.LogInformation("Compilation Time Top {CompletedTaskCount}", CompletedActions.Count());
				Logger.LogInformation("");
				foreach (int Index in CompletedActions)
				{
					IExternalAction Action = Actions[Index].Action.Inner;
					ExecuteResults Result = Actions[Index].Results!;

					string Description = $"{(Action.CommandDescription ?? Action.CommandPath.GetFileNameWithoutExtension())} {Action.StatusDescription}".Trim();
					if (Result.ProcessorTime.Ticks > 0)
					{
						Logger.LogInformation("{Description} [ Wall Time {ExecutionTime:0.00} s / CPU Time {ProcessorTime:0.00} s ]", Description, Result.ExecutionTime.TotalSeconds, Result.ProcessorTime.TotalSeconds);
					}
					else
					{
						Logger.LogInformation("{Description} [ Time {ExecutionTime:0.00} s ]", Description, Result.ExecutionTime.TotalSeconds);
					}

				}
				Logger.LogInformation("");
			}
		}

		private enum ActionReadyState
		{
			NotReady,
			Error,
			Ready,
		}

		/// <summary>
		/// Get the ready state of an action
		/// </summary>
		/// <param name="action">Action in question</param>
		/// <returns>Action ready state</returns>
		private ActionReadyState GetActionReadyState(LinkedAction action)
		{
			foreach (LinkedAction prereq in action.PrerequisiteActions)
			{

				// To avoid doing artifact checks on actions that might need compiling,
				// we first make sure the action is in the compile phase
				if (Actions[prereq.SortIndex].Phase != ActionPhase.Compile)
				{
					return ActionReadyState.NotReady;
				}

				// Respect the compile status of the action
				switch (Actions[prereq.SortIndex].Status)
				{
					case ActionStatus.Finished:
						continue;

					case ActionStatus.Error:
						return ActionReadyState.Error;

					default:
						return ActionReadyState.NotReady;
				}
			}
			return ActionReadyState.Ready;
		}
	}
}
