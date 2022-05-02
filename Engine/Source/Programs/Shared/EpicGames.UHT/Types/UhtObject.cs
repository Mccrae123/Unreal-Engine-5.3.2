// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Utils;

namespace EpicGames.UHT.Types
{
	/// <summary>
	/// Represents a UObject in the engine
	/// </summary>
	[UhtEngineClass(Name = "Object")]
	public abstract class UhtObject : UhtType
	{

		/// <summary>
		/// Internal object flags.
		/// </summary>
		public EInternalObjectFlags InternalObjectFlags { get; set; } = EInternalObjectFlags.None;

		/// <summary>
		/// Unique index of the object
		/// </summary>
		public int ObjectTypeIndex { get; }

		/// <summary>
		/// The alternate object is used by the interface system where the native interface will
		/// update this setting to point to the UInterface derived companion object.
		/// </summary>
		public UhtObject? AlternateObject { get; set; } = null;

		/// <inheritdoc/>
		public override string EngineClassName => "Object";

		/// <summary>
		/// Construct a new instance of the object
		/// </summary>
		/// <param name="session">Session of the object</param>
		protected UhtObject(UhtSession session) : base(session)
		{
			ObjectTypeIndex = this.Session.GetNextObjectTypeIndex();
		}

		/// <summary>
		/// Construct a new instance of the object
		/// </summary>
		/// <param name="outer">Outer object</param>
		/// <param name="lineNumber">Line number where object is defined</param>
		protected UhtObject(UhtType outer, int lineNumber) : base(outer, lineNumber)
		{
			ObjectTypeIndex = this.Session.GetNextObjectTypeIndex();
		}
	}
}
