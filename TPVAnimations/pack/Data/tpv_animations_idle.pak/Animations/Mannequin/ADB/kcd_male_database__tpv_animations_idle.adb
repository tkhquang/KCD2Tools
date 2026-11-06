<AnimDB FragDef="Animations/Mannequin/ADB/kcd_male_fragmentids.xml" TagDef="Animations/Mannequin/ADB/kcd_male_tags.xml">
	<FragmentList>
		<MotionIdle>
			<Fragment BlendOutDuration="0.2" GUID="1203bd60-4a1d-5957-b176-97fc56e5a4b8" Tags="player+thirdPerson">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.4" />
					<Animation name="relaxed_idle_both_player" flags="Loop" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="Variation">
						<ProceduralParams>
							<FragmentId value="MotionIdleVARdefault" />
							<From value="1" />
							<To value="3" />
							<VarPct value="100" />
							<VarNum value="7" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="AnimationOverride">
						<ProceduralParams>
							<FragmentId value="WeaponHolsterFull" />
							<FragmentIdBlendOut value="" />
							<RestartFragment value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
			</Fragment>
		</MotionIdle>
	</FragmentList>
</AnimDB>