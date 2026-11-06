<AnimDB FragDef="Animations/Mannequin/ADB/kcd_male_fragmentids.xml" TagDef="Animations/Mannequin/ADB/kcd_male_tags.xml">
	<FragmentList>
		<MotionIdle>
			<Fragment BlendOutDuration="0.2" GUID="642648c7-f9bb-5032-9e5e-b7a4832e0d0e" Tags="drunk+player+thirdPerson">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="tpv_idle_drunk" flags="Loop" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="AnimationOverride">
						<ProceduralParams>
							<FragmentId value="WeaponHolsterFull" />
							<FragmentIdBlendOut value="" />
							<RestartFragment value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="de5cae92-647e-5f97-9447-8df9b4ad10b8" Tags="l_noweapon+r_noweapon+drunk+player+thirdPerson">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.48" />
					<Animation name="tpv_idle_drunk" flags="Loop" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="AnimationOverride">
						<ProceduralParams>
							<FragmentId value="WeaponHolsterFull" />
							<FragmentIdBlendOut value="" />
							<RestartFragment value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
			</Fragment>
		</MotionIdle>
	</FragmentList>
</AnimDB>