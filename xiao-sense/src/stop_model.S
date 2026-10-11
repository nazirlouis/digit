.section .rodata
.balign 16
.global stop_model_start
stop_model_start:
.incbin STOP_MODEL_FILE
.global stop_model_end
stop_model_end:
