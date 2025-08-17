# Typical layout of the generated file:
#    Offset | File
# -  0x1000 | ~\.platformio\packages\framework-arduinoespressif32\tools\sdk\esp32\bin\bootloader_dout_40m.bin
# -  0x8000 | ~\ESPEasy\.pio\build\<env name>\partitions.bin
# -  0xe000 | ~\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin
# - 0x10000 | ~\ESPEasy\.pio\build\<env name>/<built binary>.bin

Import("env")

import sys, shutil, os
from colorama import Fore, Back, Style
from os.path import join

sys.path.append(join(env.PioPlatform().get_package_dir("tool-esptoolpy")))
import esptool


def esptool_call(cmd):
    try:
        esptool.main(cmd)
    except SystemExit as e:
        # Fetch sys.exit() without leaving the script
        if e.code == 0:
            return True
        else:
            print(f"❌ esptool failed with exit code: {e.code}")
            return False


def export_firmware_name(env):
    version_string = 'v'
    for each_defined in env['CPPDEFINES']:
        if each_defined[0] == 'FIRMWARE_MAJOR_VERSION' or each_defined[0] == 'FIRMWARE_MINOR_VERSION':
            version_string += str(each_defined[1]) + "."
        elif each_defined[0] == 'FIRMWARE_PATCH_VERSION':
            version_string += str(each_defined[1])
        elif each_defined[0] == 'FIRMWARE_BETA_VERSION':
            version_string += '-beta'

    return "gogo-firmware-" + version_string

def esp32_create_combined_bin(source, target, env):
    #print("Generating combined binary for serial flashing")
    # The offset from begin of the file where the app0 partition starts
    # This is defined in the partition .csv file
    app_offset = 0x10000     # default value for "old" scheme

    new_file_name = env.subst("$BUILD_DIR/${PROGNAME}.factory.bin")
    firmware_name = env.subst("$BUILD_DIR/${PROGNAME}.bin")

    sections = env.subst(env.get("FLASH_EXTRA_IMAGES"))
    chip = env.get("BOARD_MCU")

    flash_size = env.BoardConfig().get("upload.flash_size", "16MB")
    flash_freq = env.BoardConfig().get("build.f_flash", "40000000L")
    flash_freq = str(flash_freq).replace("L", "")
    flash_freq = str(int(int(flash_freq) / 1000000)) + "m"
    flash_mode = env.BoardConfig().get("build.flash_mode", "dio")
    memory_type = env.BoardConfig().get("build.arduino.memory_type", "qio_qspi")

    if flash_mode == "qio" or flash_mode == "qout":
        flash_mode = "dio"
    if memory_type == "opi_opi" or memory_type == "opi_qspi":
        flash_mode = "dout"
    cmd = [
        "--chip",
        chip,
        "merge-bin",
        "-o",
        new_file_name,
        "--flash-mode",
        flash_mode,
        "--flash-freq",
        flash_freq,
        "--flash-size",
        flash_size,
    ]

    print()
    print(Fore.YELLOW + "    Offset   | File")
    for section in sections:
        sect_adr, sect_file = section.split(" ", 1)
        print(Fore.RED + f" -  {sect_adr.ljust(8)} | {sect_file}")
        cmd += [sect_adr, sect_file]

    print(Fore.RED + f" -  {hex(app_offset).ljust(8)} | {firmware_name}")
    cmd += [hex(app_offset), firmware_name]

    print()
    # print(Fore.RED + 'Using esptool.py arguments: %s' % ' '.join(cmd))
    esptool_call(cmd)

    # INFO: export firmware file to `./dist`
    dist_dir = join(env["PROJECT_DIR"], "dist")
    if not os.path.exists(dist_dir):
        os.makedirs(dist_dir)

    gogo_firmware_name = export_firmware_name(env)
    combine_firmware_path = join(dist_dir, gogo_firmware_name + ".factory.bin")
    firmware_path = join(dist_dir, gogo_firmware_name + ".bin")

    shutil.copy2(new_file_name, combine_firmware_path)
    shutil.copy2(firmware_name, firmware_path)

    print(Fore.BLUE + "export firmware to ./dist/" + gogo_firmware_name + "(.factory).bin")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", esp32_create_combined_bin)
