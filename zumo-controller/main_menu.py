"""Pololu startup menu redirected to Digit's selected screen.

Newer Pololu firmware runs main_menu.py first and lets it launch main.py as the
default program. Keeping the menu here prevents two nested splash sequences.
"""


try:
    # Redirect both display import paths before the factory loader or any demo
    # program imports them.
    from digit_display import Display
    import zumo_2040_robot.display as display_module
    from zumo_2040_robot import robot

    display_module.Display = Display
    robot.Display = Display

    from zumo_2040_robot.extras.splash_loader import splash_loader

    print("DIGIT MENU: starting configured startup screen")
    splash_loader(
        default_program="main.py",
        splash_delay_s=6,
        run_file_delay_ms=700,
    )

except Exception as error:
    from zumo_2040_robot.motors import Motors
    Motors()
    exc = error
    print("DIGIT MENU ERROR:", type(error).__name__, error)
    try:
        Display.show_exception(error)
    except Exception as display_error:
        print("DIGIT MENU DISPLAY ERROR:", type(display_error).__name__, display_error)
    raise

finally:
    from zumo_2040_robot.motors import Motors
    from zumo_2040_robot.buzzer import Buzzer
    from zumo_2040_robot.rgb_leds import RGBLEDs

    Motors()
    Buzzer()
    RGBLEDs()
