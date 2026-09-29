@ Entry points of asm/transform.s used by the audio block codec (see
@ asm/transform_veneers.s). On the Vita, callers reach ARM code through BLX,
@ so the veneers are plain branches.
	.syntax unified
	.text
	.arm

	.global func_081213C4
	.type func_081213C4, %function
func_081213C4:
	b	func_08109BE0

	.global func_081213CC
	.type func_081213CC, %function
func_081213CC:
	b	func_08109AAC

	.global func_081213D4
	.type func_081213D4, %function
func_081213D4:
	b	func_08109C68
