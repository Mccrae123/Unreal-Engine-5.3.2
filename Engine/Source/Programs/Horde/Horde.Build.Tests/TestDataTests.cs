// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using EpicGames.Core;
using Horde.Build.Jobs.Graphs;
using Horde.Build.Projects;
using Horde.Build.Jobs;
using Horde.Build.Users;
using Horde.Build.Streams;
using Horde.Build.Utilities;
using HordeCommon;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using MongoDB.Bson;
using Moq;
using Horde.Build.Jobs.Templates;
using Horde.Build.Jobs.TestData;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using System.Security.Claims;
using EpicGames.BuildGraph.Expressions;
using Horde.Build.Logs;

namespace Horde.Build.Tests
{
	using ProjectId = StringId<IProject>;
	using StreamId = StringId<IStream>;
	using TemplateId = StringId<ITemplateRef>;
	using JobId = ObjectId<IJob>;

	/// <summary>
	/// Tests for the device service
	/// </summary>
	[TestClass]
	public class TestDataTests : TestSetup
	{
		const string MainStreamName = "//UE5/Main";
		readonly StreamId _mainStreamId = StreamId.Sanitize(MainStreamName);

		const string ReleaseStreamName = "//UE5/Release";
		readonly StreamId _releaseStreamId = StreamId.Sanitize(ReleaseStreamName);

		readonly IGraph _graph;

		TestDataController? _testDataController;

		// override DeviceController with valid user
		private new TestDataController TestDataController
		{
			get
			{
				if (_testDataController == null)
				{
					IUser user = UserCollection.FindOrAddUserByLoginAsync("TestUser").Result;
					_testDataController = base.TestDataController;
					ControllerContext controllerContext = new ControllerContext();
					controllerContext.HttpContext = new DefaultHttpContext();
					controllerContext.HttpContext.User = new ClaimsPrincipal(new ClaimsIdentity(
						new List<Claim> {new Claim(ServerSettings.AdminClaimType, ServerSettings.AdminClaimValue),
					new Claim(ClaimTypes.Name, "TestUser"),
					new Claim(HordeClaimTypes.UserId, user.Id.ToString()) }
						, "TestAuthType"));
					_testDataController.ControllerContext = controllerContext;

				}
				return _testDataController;
			}
		}

		public static INode MockNode(string name, IReadOnlyNodeAnnotations annotations)
		{
			Mock<INode> node = new Mock<INode>(MockBehavior.Strict);
			node.SetupGet(x => x.Name).Returns(name);
			node.SetupGet(x => x.Annotations).Returns(annotations);
			return node.Object;
		}

		async Task<IStream> CreateStreamAsync(ProjectId projectId, StreamId streamId, string streamName)
		{
			string revision = $"config:{streamId}";

			StreamConfig streamConfig = new StreamConfig { Name = streamName };
			streamConfig.Tabs.Add(new JobsTabConfig { Title = "General", Templates = new List<TemplateId> { new TemplateId("test-template") } });
			streamConfig.Templates.Add(new TemplateRefConfig { Id = new TemplateId("test-template") });
			await ConfigCollection.AddConfigAsync(revision, streamConfig);

			return Deref(await StreamCollection.TryCreateOrReplaceAsync(streamId, null, revision, projectId));
		}

		public TestDataTests()
		{
			const string ProjectConfigRevision = "projectconfig";
			ConfigCollection.AddConfigAsync(ProjectConfigRevision, new ProjectConfig { Name = "UE4" }).Wait();
			IProject project = ProjectCollection.AddOrUpdateAsync(new ProjectId("ue4"), ProjectConfigRevision, 0).Result!;

			IStream mainStream = CreateStreamAsync(project.Id, _mainStreamId, MainStreamName).Result;
			IStream releaseStream = CreateStreamAsync(project.Id, _releaseStreamId, ReleaseStreamName).Result;

			List<INode> nodes = new List<INode>();
			nodes.Add(MockNode("Update Version Files", NodeAnnotations.Empty));
			nodes.Add(MockNode("Compile UnrealHeaderTool Win64", NodeAnnotations.Empty));
			nodes.Add(MockNode("Compile ShooterGameEditor Win64", NodeAnnotations.Empty));
			nodes.Add(MockNode("Cook ShooterGame Win64", NodeAnnotations.Empty));

			Mock<INodeGroup> grp = new Mock<INodeGroup>(MockBehavior.Strict);
			grp.SetupGet(x => x.Nodes).Returns(nodes);

			Mock<IGraph> graphMock = new Mock<IGraph>(MockBehavior.Strict);
			graphMock.SetupGet(x => x.Groups).Returns(new List<INodeGroup> { grp.Object });
			_graph = graphMock.Object;
		}

		public IJob CreateJob(StreamId streamId, int change, string name, IGraph graph, TimeSpan time = default)
		{
			JobId jobId = JobId.GenerateNewId();

			List<IJobStepBatch> batches = new List<IJobStepBatch>();
			for (int groupIdx = 0; groupIdx < graph.Groups.Count; groupIdx++)
			{
				INodeGroup @group = graph.Groups[groupIdx];

				List<IJobStep> steps = new List<IJobStep>();
				for (int nodeIdx = 0; nodeIdx < @group.Nodes.Count; nodeIdx++)
				{
					SubResourceId stepId = new SubResourceId((ushort)((groupIdx * 100) + nodeIdx));

					ILogFile logFile = LogFileService.CreateLogFileAsync(jobId, null, LogType.Json).Result;

					Mock<IJobStep> step = new Mock<IJobStep>(MockBehavior.Strict);
					step.SetupGet(x => x.Id).Returns(stepId);
					step.SetupGet(x => x.NodeIdx).Returns(nodeIdx);
					step.SetupGet(x => x.LogId).Returns(logFile.Id);
					step.SetupGet(x => x.StartTimeUtc).Returns(DateTime.UtcNow + time);

					steps.Add(step.Object);
				}

				SubResourceId batchId = new SubResourceId((ushort)(groupIdx * 100));

				Mock<IJobStepBatch> batch = new Mock<IJobStepBatch>(MockBehavior.Strict);
				batch.SetupGet(x => x.Id).Returns(batchId);
				batch.SetupGet(x => x.GroupIdx).Returns(groupIdx);
				batch.SetupGet(x => x.Steps).Returns(steps);
				batches.Add(batch.Object);
			}

			Mock<IJob> job = new Mock<IJob>(MockBehavior.Strict);
			job.SetupGet(x => x.Id).Returns(jobId);
			job.SetupGet(x => x.Name).Returns(name);
			job.SetupGet(x => x.StreamId).Returns(streamId);
			job.SetupGet(x => x.TemplateId).Returns(new TemplateId("test-template"));
			job.SetupGet(x => x.Change).Returns(change);
			job.SetupGet(x => x.Batches).Returns(batches);
			job.SetupGet(x => x.ShowUgsBadges).Returns(false);
			job.SetupGet(x => x.ShowUgsAlerts).Returns(false);
			job.SetupGet(x => x.PromoteIssuesByDefault).Returns(false);
			job.SetupGet(x => x.UpdateIssues).Returns(false);
			job.SetupGet(x => x.NotificationChannel).Returns("#devtools-horde-slack-testing");
			return job.Object;
		}



		[TestMethod]
		public async Task SimpleReportTest()
		{
			if (ServerSettings.FeatureFlags.EnableTestDataV2)
			{
				string[] streamIds = new string[] { _mainStreamId.ToString(), _releaseStreamId.ToString() };

				IJob job = CreateJob(_mainStreamId, 105, "Test Build", _graph);
				IJobStep step = job.Batches[0].Steps[0];
				IJob job2 = CreateJob(_releaseStreamId, 105, "Test Build", _graph);
				IJobStep step2 = job2.Batches[0].Steps[0];

				BsonDocument testData = BsonDocument.Parse(String.Join('\n', _simpleTestDataLines));

				await TestDataCollection.AddAsync(job, step, "Simple Report Key", testData);
				await TestDataCollection.AddAsync(job2, step2, "Simple Report Key", testData);


				ActionResult<List<GetTestStreamResponse>> streamResult = await TestDataController.GetTestStreamsAsync(streamIds);
				Assert.IsNotNull(streamResult);
				Assert.IsNotNull(streamResult.Value);
				List<GetTestStreamResponse> streams = streamResult.Value;

				Assert.AreEqual(2, streams.Count);
				Assert.AreEqual(1, streams[0].Tests.Count);
				Assert.AreEqual(1, streams[0].TestMetadata.Count);
				Assert.AreEqual(0, streams[0].TestSuites.Count);
				Assert.AreEqual(1, streams[1].Tests.Count);
				Assert.AreEqual(1, streams[1].TestMetadata.Count);
				Assert.AreEqual(0, streams[1].TestSuites.Count);

				Assert.AreEqual(streams[0].TestMetadata[0].Id, streams[1].TestMetadata[0].Id);

				ActionResult<List<GetTestDataRefResponse>> refResult = await TestDataController.GetTestDataRefAsync(streamIds);
				Assert.IsNotNull(refResult);
				Assert.IsNotNull(refResult.Value);
				List<GetTestDataRefResponse> refs = refResult.Value;

				Assert.AreEqual(2, refs.Count);
			}
		}

		[TestMethod]
		public async Task SessionReportTest()
		{
			if (ServerSettings.FeatureFlags.EnableTestDataV2)
			{
				string[] streamIds = new string[] { _mainStreamId.ToString(), _releaseStreamId.ToString() };

				IJob job = CreateJob(_mainStreamId, 105, "Test Build", _graph);
				IJobStep step = job.Batches[0].Steps[0];
				IJob job2 = CreateJob(_releaseStreamId, 105, "Test Build", _graph);
				IJobStep step2 = job2.Batches[0].Steps[0];

				BsonDocument testData = BsonDocument.Parse(String.Join('\n', _testSessionDataLines));

				await TestDataCollection.AddAsync(job, step, "Session Report Key", testData);
				await TestDataCollection.AddAsync(job2, step2, "Session Report Key", testData);

				ActionResult<List<GetTestStreamResponse>> streamResult = await TestDataController.GetTestStreamsAsync(streamIds);
				Assert.IsNotNull(streamResult);
				Assert.IsNotNull(streamResult.Value);
				List<GetTestStreamResponse> streams = streamResult.Value;

				Assert.AreEqual(2, streams.Count);

				Assert.AreEqual(4, streams[0].Tests.Count);				
				Assert.AreEqual(1, streams[0].TestMetadata.Count);
				Assert.AreEqual(1, streams[0].TestSuites.Count);
				Assert.AreEqual(4, streams[0].TestSuites[0].Tests.Count);

				Assert.AreEqual(4, streams[1].Tests.Count);
				Assert.AreEqual(1, streams[1].TestMetadata.Count);
				Assert.AreEqual(1, streams[1].TestSuites.Count);
				Assert.AreEqual(4, streams[1].TestSuites[0].Tests.Count);

				Assert.AreEqual(streams[0].TestMetadata[0].Id, streams[1].TestMetadata[0].Id);

				ActionResult<List<GetTestDataRefResponse>> refResult = await TestDataController.GetTestDataRefAsync(streamIds);
				Assert.IsNotNull(refResult);
				Assert.IsNotNull(refResult.Value);
				List<GetTestDataRefResponse> refs = refResult.Value;

				Assert.AreEqual(2, refs.Count);
			}
			
		}

		private readonly string[] _simpleTestDataLines =
		{
			@"{",
			@"  ""Items"": [",
			@"    {",
			@"      ""Key"": ""Simple Report::UE.BootTest EngineTest Editor Win64"",",
			@"      ""Data"": {",
			@"		""Version"" : 1,",
			@"        ""Type"": ""Simple Report"",",
			@"        ""TestName"": ""EditorBootTest"",",
			@"        ""Description"": ""Win64 Development EditorGame"",",
			@"        ""ReportCreatedOn"": ""10/31/2022 11:44:56 AM"",",
			@"        ""TotalDurationSeconds"": 35.33694,",
			@"        ""HasSucceeded"": true,",
			@"        ""Status"": ""Passed"",",
			@"        ""URLLink"": """",",
			@"        ""BuildChangeList"": 22815797,",
			@"        ""MainRole"": {",
			@"          ""Type"": ""Editor"",",
			@"          ""Platform"": ""Win64"",",
			@"          ""Configuration"": ""Development""",
			@"        },",
			@"        ""Roles"": [",
			@"          {",
			@"            ""Type"": ""Editor"",",
			@"            ""Platform"": ""Win64"",",
			@"            ""Configuration"": ""Development""",
			@"          }",
			@"        ],",
			@"        ""TestResult"": ""Passed"",",
			@"        ""Logs"": [],",
			@"        ""Errors"": [],",
			@"        ""Warnings"": [],",
			@"        ""Metadata"": {",
			@"          ""Platform"": ""Win64"",",
			@"          ""BuildTarget"": ""Editor"",",
			@"          ""Configuration"": ""Development"",",
			@"          ""Project"": ""EngineTest""",
			@"        }",
			@"      }",
			@"    }",
			@"  ],",
			@"  ""Version"": 1",
			@"}"
		};

		private readonly string[] _testSessionDataLines =
		{
			@"{",
			@"	""Version"": 1,",
			@"    ""Items"": [	",
			@"        {",
			@"            ""Key"": ""Automated Test Session"",",
			@"            ""Data"": {",
			@"                ""Type"": ""Automated Test Session"",",
			@"                ""Name"": ""UE.Automation(Group:UI) EngineTest"",",
			@"                ""PreFlightChange"": """",",
			@"                ""TestSessionInfo"": {",
			@"                    ""DateTime"": ""2022.10.31-01.35.07"",",
			@"                    ""TimeElapseSec"": 17.63286,",
			@"                    ""Tests"": {",
			@"                        ""cbdb55ea"": {",
			@"                            ""Name"": ""Project.Functional Tests.Tests.UI.Clipping.WidgetClipping.Simple UI"",",
			@"                            ""TestUID"": ""cbdb55ea"",",
			@"                            ""Suite"": ""UI"",",
			@"                            ""State"": ""Skipped"",",
			@"                            ""DeviceAppInstanceName"": [",
			@"                                ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042""",
			@"                            ],",
			@"                            ""ErrorCount"": 0,",
			@"                            ""WarningCount"": 0,",
			@"                            ""ErrorHashAggregate"": """",",
			@"                            ""DateTime"": ""2022.10.31-01.34.41"",",
			@"                            ""TimeElapseSec"": 0",
			@"                        },",
			@"                        ""eafa1362"": {",
			@"                            ""Name"": ""Project.Functional Tests.Tests.UI.Effects.UI_Effects.Blur"",",
			@"                            ""TestUID"": ""eafa1362"",",
			@"                            ""Suite"": ""UI"",",
			@"                            ""State"": ""Success"",",
			@"                            ""DeviceAppInstanceName"": [",
			@"                                ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042""",
			@"                            ],",
			@"                            ""ErrorCount"": 0,",
			@"                            ""WarningCount"": 0,",
			@"                            ""ErrorHashAggregate"": """",",
			@"                            ""DateTime"": ""2022.10.31-01.34.49"",",
			@"                            ""TimeElapseSec"": 10.5334",
			@"                        },",
			@"                        ""1521079c"": {",
			@"                            ""Name"": ""Project.Functional Tests.Tests.UI.Fonts.FontOutlineTestUI.FontOutlineTest"",",
			@"                            ""TestUID"": ""1521079c"",",
			@"                            ""Suite"": ""UI"",",
			@"                            ""State"": ""Success"",",
			@"                            ""DeviceAppInstanceName"": [",
			@"                                ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042""",
			@"                            ],",
			@"                            ""ErrorCount"": 0,",
			@"                            ""WarningCount"": 0,",
			@"                            ""ErrorHashAggregate"": """",",
			@"                            ""DateTime"": ""2022.10.31-01.35.00"",",
			@"                            ""TimeElapseSec"": 3.53325",
			@"                        },",
			@"                        ""c2638213"": {",
			@"                            ""Name"": ""Project.Functional Tests.Tests.UI.Fonts.FontRenderingTestUI.FontRenderingTest"",",
			@"                            ""TestUID"": ""c2638213"",",
			@"                            ""Suite"": ""UI"",",
			@"                            ""State"": ""Fail"",",
			@"                            ""DeviceAppInstanceName"": [",
			@"                                ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042""",
			@"                            ],",
			@"                            ""ErrorCount"": 1,",
			@"                            ""WarningCount"": 2,",
			@"                            ""ErrorHashAggregate"": """",",
			@"                            ""DateTime"": ""2022.10.31-01.35.03"",",
			@"                            ""TimeElapseSec"": 3.56621",
			@"                        }",
			@"                    },",
			@"                    ""TestResultsTestDataUID"": ""b420bdde-c030-4add-81d2-3a8404ab3e45""",
			@"                },",
			@"                ""Devices"": [",
			@"                    {",
			@"                        ""Name"": ""RDU-WIN64-12"",",
			@"                        ""AppInstanceName"": ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042"",",
			@"                        ""AppInstanceLog"": ""UI_Win64_Game/Client/ClientOutput.log"",",
			@"                        ""Metadata"": {",
			@"                            ""platform"": ""Win64"",",
			@"                            ""os_version"": ""14 3"",",
			@"                            ""model"": ""Default"",",
			@"                            ""gpu"": ""Win64 GPU"",",
			@"                            ""cpumodel"": ""Win64 CPU"",",
			@"                            ""ram_in_gb"": ""5"",",
			@"                            ""render_mode"": ""ES3_1"",",
			@"                            ""rhi"": """"",
			@"                        }",
			@"                    }",
			@"                ],",
			@"                ""IndexedErrors"": {},",
			@"                ""Metadata"": {",
			@"                    ""Platform"": ""Win64"",",
			@"                    ""BuildTarget"": ""Client"",",
			@"                    ""Configuration"": ""Development"",",
			@"                    ""Project"": ""EngineTest"",",
			@"                    ""RHI"": ""default""",
			@"                }",
			@"            }",
			@"        },",
			@"        {",
			@"            ""Key"": ""Unreal Automated Tests::UE.TargetAutomation(RunTest=UI) Win64"",",
			@"            ""Data"": {",
			@"                ""Type"": ""Unreal Automated Tests"",",
			@"                ""Devices"": [",
			@"                    {",
			@"                        ""DeviceName"": ""RDU-WIN64-12"",",
			@"                        ""Instance"": ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042"",",
			@"                        ""Platform"": ""Win64"",",
			@"                        ""OSVersion"": ""14 3"",",
			@"                        ""Model"": ""Default"",",
			@"                        ""GPU"": ""Win64 GPU"",",
			@"                        ""CPUModel"": ""Win64 CPU"",",
			@"                        ""RAMInGB"": 5,",
			@"                        ""RenderMode"": ""ES3_1"",",
			@"                        ""RHI"": """"",
			@"                    }",
			@"                ],",
			@"                ""ReportCreatedOn"": ""2022.10.31-01.35.07"",",
			@"                ""ReportURL"": """",",
			@"                ""SucceededCount"": 3,",
			@"                ""SucceededWithWarningsCount"": 0,",
			@"                ""FailedCount"": 0,",
			@"                ""NotRunCount"": 0,",
			@"                ""InProcessCount"": 0,",
			@"                ""TotalDurationSeconds"": 17.63286,",
			@"                ""Tests"": [",
			@"                    {",
			@"                        ""TestDisplayName"": ""Simple UI"",",
			@"                        ""FullTestPath"": ""Project.Functional Tests.Tests.UI.Clipping.WidgetClipping.Simple UI"",",
			@"                        ""State"": ""Skipped"",",
			@"                        ""DeviceInstance"": ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042"",",
			@"                        ""Errors"": 0,",
			@"                        ""Warnings"": 0,",
			@"                        ""ArtifactName"": ""33ec1c33-71b7-41a1-84ba-d5ee2f42af8c.json""",
			@"                    },",
			@"                    {",
			@"                        ""TestDisplayName"": ""Blur"",",
			@"                        ""FullTestPath"": ""Project.Functional Tests.Tests.UI.Effects.UI_Effects.Blur"",",
			@"                        ""State"": ""Success"",",
			@"                        ""DeviceInstance"": ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042"",",
			@"                        ""Errors"": 0,",
			@"                        ""Warnings"": 0,",
			@"                        ""ArtifactName"": ""450130ac-18cb-4bee-b529-c1d8ae943a61.json""",
			@"                    },",
			@"                    {",
			@"                        ""TestDisplayName"": ""FontOutlineTest"",",
			@"                        ""FullTestPath"": ""Project.Functional Tests.Tests.UI.Fonts.FontOutlineTestUI.FontOutlineTest"",",
			@"                        ""State"": ""Success"",",
			@"                        ""DeviceInstance"": ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042"",",
			@"                        ""Errors"": 0,",
			@"                        ""Warnings"": 0,",
			@"                        ""ArtifactName"": ""12752762-6332-462f-8ef3-254c7502b57a.json""",
			@"                    },",
			@"                    {",
			@"                        ""TestDisplayName"": ""FontRenderingTest"",",
			@"                        ""FullTestPath"": ""Project.Functional Tests.Tests.UI.Fonts.FontRenderingTestUI.FontRenderingTest"",",
			@"                        ""State"": ""Success"",",
			@"                        ""DeviceInstance"": ""3da3633f-c82a-5c25-9a60-1e852705b65e-C0218E40D9AC4B4FBD07326E10179042"",",
			@"                        ""Errors"": 0,",
			@"                        ""Warnings"": 0,",
			@"                        ""ArtifactName"": ""711a5bd9-26dd-470b-a3c6-98f04a522e98.json""",
			@"                    }",
			@"                ],",
			@"                ""Metadata"": {",
			@"                    ""Platform"": ""Win64"",",
			@"                    ""BuildTarget"": ""Client"",",
			@"                    ""Configuration"": ""Development"",",
			@"                    ""Project"": ""EngineTest""",
			@"                }",
			@"            }",
			@"        },",
			@"        {",
			@"            ""Key"": ""Automated Test Session Result Details::b420bdde-c030-4add-81d2-3a8404ab3e45"",",
			@"            ""Data"": {",
			@"                ""cbdb55ea"": {",
			@"                    ""Events"": [",
			@"                        {",
			@"                            ""Message"": ""Skipping test because of exclude list: Sporadic failure with animated UI elements. Jira UE-113396"",",
			@"                            ""Context"": """",",
			@"                            ""Type"": ""Info"",",
			@"                            ""Tag"": ""entry"",",
			@"                            ""Hash"": """",",
			@"                            ""DateTime"": ""0001.01.01-00.00.00"",",
			@"                            ""Artifacts"": []",
			@"                        }",
			@"                    ]",
			@"                },",
			@"                ""eafa1362"": {",
			@"                    ""Events"": [",
			@"                        {",
			@"                            ""Message"": ""LogGauntlet: GauntletHeartbeat: Idle "",",
			@"                            ""Context"": """",",
			@"                            ""Type"": ""Info"",",
			@"                            ""Tag"": ""entry"",",
			@"                            ""Hash"": """",",
			@"                            ""DateTime"": ""0001.01.01-00.00.00"",",
			@"                            ""Artifacts"": []",
			@"                        },",
			@"                        {",
			@"                            ""Message"": ""Screenshot 'Blur' was similar!  Global Difference = 0.000303, Max Local Difference = 0.005257"",",
			@"                            ""Context"": ""UI_Effects/Blur"",",
			@"                            ""Type"": ""Info"",",
			@"                            ""Tag"": ""image comparison"",",
			@"                            ""Hash"": """",",
			@"                            ""DateTime"": ""0001.01.01-00.00.00"",",
			@"                            ""Artifacts"": [",
			@"                                {",
			@"                                    ""Tag"": ""unapproved"",",
			@"                                    ""ReferencePath"": ""imageCompare/UI_Effects/Blur/Win64/ES3_1/Incoming.png""",
			@"                                }",
			@"                            ]",
			@"                        }",
			@"                    ]",
			@"                },",
			@"                ""1521079c"": {",
			@"                    ""Events"": [",
			@"                        {",
			@"                            ""Message"": ""Screenshot 'FontOutlineTest' was similar!  Global Difference = 0.000083, Max Local Difference = 0.003520"",",
			@"                            ""Context"": ""FontOutlineTestUI/FontOutlineTest"",",
			@"                            ""Type"": ""Info"",",
			@"                            ""Tag"": ""image comparison"",",
			@"                            ""Hash"": """",",
			@"                            ""DateTime"": ""0001.01.01-00.00.00"",",
			@"                            ""Artifacts"": [",
			@"                                {",
			@"                                    ""Tag"": ""unapproved"",",
			@"                                    ""ReferencePath"": ""imageCompare/FontOutlineTestUI/FontOutlineTest/Win64/ES3_1/Incoming.png""",
			@"                                }",
			@"                            ]",
			@"                        }",
			@"                    ]",
			@"                },",
			@"                ""c2638213"": {",
			@"                    ""Events"": [",
			@"                        {",
			@"                            ""Message"": ""Screenshot 'FontRenderingTest' was similar!  Global Difference = 0.000000, Max Local Difference = 0.000000"",",
			@"                            ""Context"": ""FontRenderingTestUI/FontRenderingTest"",",
			@"                            ""Type"": ""Info"",",
			@"                            ""Tag"": ""image comparison"",",
			@"                            ""Hash"": """",",
			@"                            ""DateTime"": ""0001.01.01-00.00.00"",",
			@"                            ""Artifacts"": [",
			@"                                {",
			@"                                    ""Tag"": ""unapproved"",",
			@"                                    ""ReferencePath"": ""imageCompare/FontRenderingTestUI/FontRenderingTest/Win64/ES3_1/Incoming.png""",
			@"                                }",
			@"                            ]",
			@"                        }",
			@"                    ]",
			@"                }",
			@"            }",
			@"        }",
			@"    ]",
			@"}"
		};
	}
}
