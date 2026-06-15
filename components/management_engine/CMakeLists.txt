# 1. Реєструємо компонент.
# ВАЖЛИВО: використовуємо "log", а не "esp_log"
idf_component_register(SRCS "management_engine.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_hw_support ulp log esp_system)

# 2. Магія ULP (LP-ядра).
# "me_core" — назва програми LP-ядра
# "ulp/lp_core_main.c" — вихідний код для LP-ядра
# "management_engine.c" — вказуємо, що цей файл HP-ядра чекає на хедер від ULP
ulp_embed_binary("me_core" "ulp/lp_core_main.c" "management_engine.c")
