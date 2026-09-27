.section .rodata
.balign 16
.global digit_model_start
digit_model_start:
.incbin "wake_word_digit/digit.tflite"
.global digit_model_end
digit_model_end:
