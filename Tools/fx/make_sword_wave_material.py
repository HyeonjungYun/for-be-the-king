"""Create /Game/FX/M_S1SwordWave - the glow of a sword wave (검의 찬양 1009 vertical crescent, 2026-10-15).

Unlit · translucent · two-sided. Colour and alpha come from the mesh's vertex colours (the client builds the crescent and
paints its leading edge white), so one material serves every wave. "Intensity" scales the colour, "Fade" (1 → 0) the
opacity - the client fades it out through a dynamic instance. Translucent, not additive: the floor is pale, and additive
light on a pale floor washes every colour to white (first live check, 2026-10-15).
Run headless:  UnrealEditor-Cmd S1.uproject -run=pythonscript -script=Tools/fx/make_sword_wave_material.py -unattended -nullrhi
"""
import unreal

PATH = "/Game/FX"
NAME = "M_S1SwordWave"
FULL = PATH + "/" + NAME

lib = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
if lib.does_asset_exist(FULL):
    lib.delete_asset(FULL)
mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())
mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
mat.set_editor_property("two_sided", True)

vc = mel.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -700, 0)
intensity = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -700, 250)
intensity.set_editor_property("parameter_name", "Intensity")
intensity.set_editor_property("default_value", 1.6)
fade = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -700, 400)
fade.set_editor_property("parameter_name", "Fade")
fade.set_editor_property("default_value", 1.0)

glow = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -400, 0)
mel.connect_material_expressions(vc, "", glow, "A")
mel.connect_material_expressions(intensity, "", glow, "B")
mel.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
alpha = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -400, 300)
mel.connect_material_expressions(vc, "A", alpha, "A")
mel.connect_material_expressions(fade, "", alpha, "B")
mel.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)

mel.recompile_material(mat)
lib.save_asset(FULL)
unreal.log("make_sword_wave_material: saved %s (expressions=%d)" % (FULL, mel.get_num_material_expressions(mat)))
