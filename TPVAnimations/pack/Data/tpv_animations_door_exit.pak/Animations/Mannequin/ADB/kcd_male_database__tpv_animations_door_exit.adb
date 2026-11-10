<AnimDB FragDef="Animations/Mannequin/ADB/kcd_male_fragmentids.xml" TagDef="Animations/Mannequin/ADB/kcd_male_tags.xml">
	<FragmentList>
		<AnimationControlled>
			<Fragment BlendOutDuration="0.2" GUID="dc560bff-1ae6-5506-a786-6f0906e480fe" Tags="stealth+player+thirdPerson" FragTags="door_r_f_c">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="stealth_door_r_f_c_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.43112" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.7096986" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.325" StartTime="0" Duration="0.7666667" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.2" StartTime="0" Duration="0.3432636" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.5" StartTime="0" Duration="0.1377646" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="1cfb0625-64dc-5969-ad9d-2e05a3d83a09" Tags="player+thirdPerson" FragTags="door_l_f_o">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2054675" />
					<Animation name="door_l_f_o_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2035035" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3548958" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="0.9" StartTime="0" Duration="0.8100865" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.2" StartTime="0" Duration="0" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="true" />
							<UseRightHand value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.1" StartTime="0" Duration="0.2027469" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="LftArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="0.75" StartTime="0" Duration="0.4804314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="99cc6824-a3f8-589c-8336-a616e9f39363" Tags="player+thirdPerson" FragTags="door_r_f_c+gateSmall">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_small_close_front_right" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.81" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="f48f32f7-2a59-5c02-8cee-aaedf7d82cab" Tags="player+thirdPerson" FragTags="door_l_f_c+gateSmall">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_small_close_front_left" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.81" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="true" />
							<UseRightHand value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="df25ee24-2518-5041-898c-ef2305171141" Tags="player+thirdPerson" FragTags="door_r_b_o+gateBig">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_long_open_back_right" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.71" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="cb72e4d5-51a1-5b9f-957a-4aeccd78aedc" Tags="player+thirdPerson" FragTags="door_l_f_o+gateSmall">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_small_open_front_left" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.89" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="true" />
							<UseRightHand value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="837fe05a-fcc8-5fc4-a488-9d5c9ff3e88e" Tags="player+thirdPerson" FragTags="door_l_b_c+gateSmall">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_small_close_back_left" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="ed1f03cf-1771-57f0-9b2c-0086a2ce8663" Tags="player+thirdPerson" FragTags="door_l_f_o+gateBig">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_long_open_front_left" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.89" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="true" />
							<UseRightHand value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="61950cdd-54df-5dbc-99fd-e591db676575" Tags="player+thirdPerson" FragTags="door_l_b_c+gateBig">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_long_close_back_left" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="7b29f486-d16b-537c-a458-89b8e26a9036" Tags="player+thirdPerson" FragTags="door_r_b_o">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2516803" />
					<Animation name="door_r_b_o_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2017012" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.5769488" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.1" StartTime="0" Duration="0.8806577" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.2" StartTime="0" Duration="0" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3030401" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.05000001" StartTime="0" Duration="0.1518988" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="-0.01" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.05" StartTime="0" Duration="0.4804314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="be289dfa-2b56-5594-923b-776817d80534" Tags="stealth+player+thirdPerson" FragTags="door_l_f_o">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="stealth_door_l_f_o_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3055199" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.9887999" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.2" StartTime="0" Duration="1.12224" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.3" StartTime="0" Duration="0.1759999" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0.01" />
								<Element value="0.02" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.1" StartTime="0" Duration="0.1604314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="d17983b6-b4ad-5501-bf57-e5dee9d00495" Tags="player+thirdPerson" FragTags="door_l_b_o+gateSmall">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_small_open_back_left" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.7" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="04e74877-788a-5a2c-a38e-e1282e3b8a5b" Tags="stealth+player+thirdPerson" FragTags="door_l_f_c">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="stealth_door_l_f_c_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.30104" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.7006399" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="0.8" StartTime="0" Duration="0.8" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.2" StartTime="0" Duration="0.4078988" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="-0.06" />
								<Element value="0" />
								<Element value="-0.04" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="0.8" StartTime="0" Duration="0.4804314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="c5437efc-6cf2-5dd6-bbad-e53aa5a7564e" Tags="stealth+player+thirdPerson" FragTags="door_r_b_c">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="stealth_door_r_b_c_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.9" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="2.1" StartTime="0" Duration="0.6333334" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.6" StartTime="0" Duration="0.2440588" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="-0.01" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.25" StartTime="0" Duration="0.4804314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="c527858d-66c5-5288-872f-c5fbd20be5b2" Tags="player+thirdPerson" FragTags="door_l_f_c">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3188816" />
					<Animation name="door_l_f_c_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.6935832" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.034807" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.475" StartTime="0" Duration="0.8321537" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.09775051" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.09911609" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="true" />
							<UseRightHand value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.1009272" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.55" StartTime="0" Duration="0.2027469" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="LftArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="0.9999999" StartTime="0" Duration="0.2623277" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="0c2be540-2094-5292-915b-6c5d21447f18" Tags="player+thirdPerson" FragTags="door_r_f_o+gateBig">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_long_open_front_right" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.62" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="4f52044c-849f-5497-9f18-706a6f3f20e2" Tags="player+thirdPerson" FragTags="door_r_f_c">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3283479" />
					<Animation name="door_r_f_c_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2277472" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.307951" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.307951" StartTime="0" Duration="1.212752" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.55" StartTime="0" Duration="0.5406668" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.15" StartTime="0" Duration="0.4804314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="21c99ae9-037c-5fa9-ad93-fadb9a7caa1a" Tags="stealth+player+thirdPerson" FragTags="door_r_f_o">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="stealth_door_r_f_o_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.62" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.8" StartTime="0" Duration="0.5333333" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.3000001" StartTime="0" Duration="0.3218186" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0" />
								<Element value="0.02" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.5" StartTime="0" Duration="0.4804314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="ef3ff4e1-ddb1-54cd-89ba-c754c859e4bb" Tags="player+thirdPerson" FragTags="door_r_b_c+gateSmall">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_small_close_back_right" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.9" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="true" />
							<UseRightHand value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="31e2a1fd-a54d-5b92-9a3d-c8457f949541" Tags="player+thirdPerson" FragTags="door_l_b_c">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2050463" />
					<Animation name="door_l_b_c_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.4113344" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.8055999" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.05" StartTime="0" Duration="1.602904" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.4113344" StartTime="0" Duration="0.2143772" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.588666" StartTime="0" Duration="0.3135196" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="2f9f383b-954c-5219-8878-9f288dc826e6" Tags="player+thirdPerson" FragTags="door_r_b_c+gateBig">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_long_close_back_right" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.9" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="true" />
							<UseRightHand value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="29707e19-3722-59be-9d52-f30c3720e6ca" Tags="player+thirdPerson" FragTags="door_l_f_c+gateBig">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_long_close_front_left" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.81" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="true" />
							<UseRightHand value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="5ec49e72-dee2-5a86-9807-0b489667032f" Tags="stealth+player+thirdPerson" FragTags="door_l_b_o">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="stealth_door_l_b_o_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.35368" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.5" StartTime="0" Duration="0.8" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.07499999" StartTime="0" Duration="0.1271947" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="-0.02" />
								<Element value="0.02" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.1" StartTime="0" Duration="0.4804314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="70076473-20ba-5a70-9c90-535afa039879" Tags="player+thirdPerson" FragTags="door_r_f_o">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3042708" />
					<Animation name="door_r_f_o_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2022151" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.6134143" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.175" StartTime="0" Duration="0.7345366" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.2" StartTime="0" Duration="0" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.07595082" StartTime="0" Duration="0.3952908" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.075" StartTime="0" Duration="0.4804314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="c88869c7-5b47-52af-8c8d-6fe3142e25c9" Tags="player+thirdPerson" FragTags="door_l_b_o">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3055129" />
					<Animation name="door_l_b_o_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.1625049" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.6443889" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="0.8" StartTime="0" Duration="1.110276" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.073728" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.2280409" StartTime="0" Duration="0.2153535" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.021959" StartTime="0" Duration="0.6435472" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="9f049662-7994-58ad-9cfa-95ccb85c5cd2" Tags="player+thirdPerson" FragTags="door_r_b_c">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.5259998" />
					<Animation name="door_r_b_c_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3096" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.7005598" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="0.8235998" StartTime="0" Duration="1.772347" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="true" />
							<UseRightHand value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="d75e8849-ad75-5cf8-81ff-7ed31e9a67f2" Tags="stealth+player+thirdPerson" FragTags="door_l_b_c">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="stealth_door_l_b_c_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3023407" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.9887999" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.8" StartTime="0" Duration="1.15" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.4324207" StartTime="0" Duration="0.1536753" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="0" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.617579" StartTime="0" Duration="0.3801843" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="cdca2ade-532a-5aa1-92c0-4574c2adeabf" Tags="player+thirdPerson" FragTags="door_r_b_o+gateSmall">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_small_open_back_right" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.71" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="defc1dba-6083-5e03-98ce-c5e32379627e" Tags="stealth+player+thirdPerson" FragTags="door_r_b_o">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="stealth_door_r_b_o_player" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.71" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.71" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="1.2" StartTime="0" Duration="0.5366668" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0.2" StartTime="0" Duration="0.3874187" />
					<Procedural type="IKTarget">
						<ProceduralParams>
							<IKHandle value="RgtArm01" />
							<JointId value="slt_lock" />
							<Offset>
								<Element value="-0.01" />
								<Element value="0" />
								<Element value="0" />
							</Offset>
						</ProceduralParams>
					</Procedural>
					<Blend ExitTime="0.9" StartTime="0" Duration="0.4804314" />
					<Procedural type="" />
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="cc391ee5-0976-5e98-b733-acefa6cf926f" Tags="player+thirdPerson" FragTags="door_r_f_o+gateSmall">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_small_open_front_right" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.62" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="83070b64-e1fd-5c39-ab1c-a049f575e0d1" Tags="player+thirdPerson" FragTags="door_r_f_c+gateBig">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_long_close_front_right" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.81" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
			<Fragment BlendOutDuration="0.2" GUID="8a1ea699-7a4d-57a6-8ec5-5f01d81cf7d0" Tags="player+thirdPerson" FragTags="door_l_b_o+gateBig">
				<AnimLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Animation name="gate_long_open_back_left" />
				</AnimLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="MovementControlMethod">
						<ProceduralParams>
							<Horizontal value="2" />
							<Vertical value="0" />
							<XyMove value="0" />
							<ZMove value="0" />
							<Rotate value="0" />
							<Velocity value="0" />
							<Inertia value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.7" />
					<Procedural type="PositionAdjustAlignBone">
						<ProceduralParams>
							<UseZAxis value="true" />
							<RotateInAll value="false" />
							<AlignWOBone value="false" />
							<ForceAfterBlendIn value="false" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="1.04" />
					<Procedural type="AnimateCamera">
						<ProceduralParams>
							<Offset value="0" />
							<BlendPos value="0" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.2" />
					<Procedural type="ColliderMode">
						<ProceduralParams>
							<ColliderMode value="Interactive" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="UseHand">
						<ProceduralParams>
							<UseLeftHand value="false" />
							<UseRightHand value="true" />
						</ProceduralParams>
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="DisableSpeedLimiter">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
				<ProcLayer>
					<Blend ExitTime="0" StartTime="0" Duration="0.3" />
					<Procedural type="MovementRestriction">
						<ProceduralParams />
					</Procedural>
				</ProcLayer>
			</Fragment>
		</AnimationControlled>
	</FragmentList>
</AnimDB>