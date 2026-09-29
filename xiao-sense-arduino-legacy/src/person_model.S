.section .rodata
.balign 16
.global person_model_start
person_model_start:
.incbin "person_detect.tflite"
.global person_model_end
person_model_end:
