# Monster artwork generation

Generated with the built-in image_gen tool. These are right-facing idle appearance candidates, not animation sheets. Original generated files were preserved. No gameplay code or monster catalogs were changed.

## Selected images

- rusted_armor_soldier_idle.png: ordinary enemy, rusted armor and short sword.
- fallen_citadel_warden_idle.png: boss, heavy brass-trimmed armor, burgundy cloak and greatsword.

## Ordinary enemy prompt

Use case: style-transfer.
Asset type: transparent PNG enemy idle sprite for an existing 2D pixel-art action RPG.
Input images: Image 1 player_idle.png is a STYLE, PIXEL SCALE and CAMERA reference only; Image 2 town_capital_central.png is a WORLD ARCHITECTURE / PALETTE reference only. Derive a NEW enemy character; replace the human protagonist completely. Do not reproduce the player.
Primary request: one ordinary enemy called Rusted Armor Soldier, an animated hollow suit of medieval armor guarding a ruined cathedral. A compact lean humanoid slightly shorter than the reference protagonist, helmet with a single narrow dark slit and two tiny dull ember-red eyes, weathered charcoal iron armor with restrained desaturated purple shadows, rusty brown joints, a faded dark navy short tabard with tiny old brass fittings. A simple short sword held lowered in the near/right hand, other gauntleted hand slightly open at the side; no shield. Clear readable silhouette and simple armor design suitable for later animation. Two arms, two separately readable legs, intact hand gripping the sword, anatomically coherent joints.
Pose / camera: full body relaxed combat-ready standing idle, grounded feet with a small gap, knees only slightly bent; same three-quarter side view looking to screen RIGHT as the player reference. Eye-level 2D belt-scroller view, not top-down or isometric, not facing front. Entire head, feet, hands and sword visible with transparent padding on all sides, centered horizontally with feet at the lower baseline. Single sprite only.
Style: match the reference protagonist's crisp anime-influenced hand-pixeled game sprite, visible square pixel clusters, dark narrow contour, 3 to 5 discrete shades per material, sharp nearest-neighbor pixel edges, modest head size and similar body proportions. Pixel structure must be equally coarse as the protagonist, as if authored around a 96-to-128-pixel tall logical sprite and enlarged with nearest neighbor. Do not render tiny intricate high-resolution texture or smooth illustration. Top-left light, avoid thick gray or white halo outlines.
Backdrop: genuine alpha transparency; absolutely no background, floor, scenery, shadow blob, checkerboard drawing, caption, name label, text, watermark or interface.
Constraints: this is a ordinary low-tier monster, restrained silhouette and detail. Exactly one character, one idle pose. Only the new enemy is output, references are not output. Preserve actual transparency.

## Selected boss prompt

Use case: precise-object-edit.
Input: the clean isolated Rusted Armor Soldier sprite. Turn this soldier into the boss version called Fallen Citadel Warden while retaining its EXACT coarse pixel-art rendering method, right-facing 3/4 camera, narrow black pixel outline, and empty transparent canvas treatment.
Replace the enemy's equipment and proportions: much wider broad shoulders, thick heavy dark iron armor with muted violet shadows and old brass trim, a small broken crown crest integrated into the closed helmet, tiny solid red eye pixels in the slit, a short ragged burgundy cloak behind the body and arms, dark navy waist tabard with a simple brass heraldic mark. Increase muscular armor mass and bulk, with fully visible separate articulated legs and both gauntleted arms. Replace the short sword with a wide heavy straight greatsword held lowered diagonally toward the right, entire weapon within the canvas.
Standing idle with feet apart, looking right. Full body, one boss, no other subjects, no labels. Preserve the compact stylized retro game appearance; flat hard-edged shading, large obvious square pixel blocks. Same coarse logical pixel size as the input, no fine painted detail. Do not add extra detail to the surface. The whole silhouette should fit comfortably on a square canvas with generous completely empty transparent space around every side and below the feet. Sprite occupies about 80% of image height.
CRITICAL: image is ONLY an opaque pixel character cutout on fully transparent alpha. Outside the outline alpha must be zero everywhere. No shadows, colored backdrop, haze, halo, fog, glow, gradient, spotlights, vignette or atmosphere anywhere. No black rectangle and no checkerboard drawing. No cinematic concept art. No ground. The eye red is just 2-3 solid pixel blocks, not emissive light. Use the input's clean transparency, no surrounding effects.

The selected boss was derived from the selected ordinary enemy image. Earlier tall boss variants were discarded because their surrounding atmosphere was unsuitable for a sprite.

