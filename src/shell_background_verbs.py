"""Register DeskTidy items into Explorer's desktop background context menu.



Uses HKCU Directory\\Background\\shell as a single cascading submenu so Win11's

modern menu and「显示更多选项」classic menu keep system items visible.

"""



from __future__ import annotations



import sys

import winreg

from pathlib import Path

from src.settings import APP_NAME



_ROOT = r"Software\Classes\Directory\Background\shell"

_PARENT = "DeskTidy"



# child_key -> (menu label, --shell-verb id)

_CHILDREN: dict[str, tuple[str, str]] = {

    "new_fence": ("新建分区", "new-fence"),

    "refresh_fences": ("刷新所有分区", "refresh-fences"),

    "show_window": ("显示主窗口", "show-window"),

}



# Flat verbs from older builds (must be removed so they do not crowd the menu).

_OBSOLETE_FLAT_VERBS: tuple[str, ...] = (

    "DeskTidy.new_fence",

    "DeskTidy.region_fence",

    "DeskTidy.refresh_fences",

    "DeskTidy.show_window",

)





def _exe_command(verb: str) -> str:

    if getattr(sys, "frozen", False):

        exe = str(Path(sys.executable).resolve())

        return f'"{exe}" --shell-verb={verb}'

    py = str(Path(sys.executable).resolve())

    main = str((Path(__file__).resolve().parent.parent / "main.py"))

    return f'"{py}" "{main}" --shell-verb={verb}'





def _delete_key_tree(root: str) -> None:

    """Delete a registry key and its immediate ``command`` / ``shell\\*`` children."""

    # Children under shell\<parent>\shell\<child>\command

    shell_sub = root + r"\shell"

    try:

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, shell_sub) as sk:

            i = 0

            names: list[str] = []

            while True:

                try:

                    names.append(winreg.EnumKey(sk, i))

                    i += 1

                except OSError:

                    break

        for name in names:

            child = f"{shell_sub}\\{name}"

            try:

                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, child + r"\command")

            except FileNotFoundError:

                pass

            except OSError:

                pass

            try:

                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, child)

            except FileNotFoundError:

                pass

            except OSError:

                pass

        try:

            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, shell_sub)

        except FileNotFoundError:

            pass

        except OSError:

            pass

    except FileNotFoundError:

        pass

    except OSError:

        pass

    try:

        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, root + r"\command")

    except FileNotFoundError:

        pass

    except OSError:

        pass

    try:

        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, root)

    except FileNotFoundError:

        pass

    except OSError:

        pass





def _delete_flat_obsolete() -> None:

    for name in _OBSOLETE_FLAT_VERBS:

        root = f"{_ROOT}\\{name}"

        try:

            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, root + r"\command")

        except FileNotFoundError:

            pass

        except OSError:

            pass

        try:

            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, root)

        except FileNotFoundError:

            pass

        except OSError:

            pass





def register_desktop_background_verbs() -> None:

    """Install / refresh a single cascading DeskTidy submenu."""

    _delete_flat_obsolete()

    parent = f"{_ROOT}\\{_PARENT}"

    # Recreate parent cleanly so old flat/command leftovers cannot break cascading.

    _delete_key_tree(parent)



    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, parent) as key:

        winreg.SetValueEx(key, None, 0, winreg.REG_SZ, APP_NAME)

        winreg.SetValueEx(key, "MUIVerb", 0, winreg.REG_SZ, APP_NAME)

        # Empty SubCommands → use nested shell\\* children (cascade menu).

        winreg.SetValueEx(key, "SubCommands", 0, winreg.REG_SZ, "")

        winreg.SetValueEx(key, "Position", 0, winreg.REG_SZ, "Top")

        try:

            if getattr(sys, "frozen", False):

                winreg.SetValueEx(

                    key, "Icon", 0, winreg.REG_SZ, str(Path(sys.executable).resolve())

                )

        except OSError:

            pass



    for child_key, (label, verb) in _CHILDREN.items():

        child_path = f"{parent}\\shell\\{child_key}"

        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, child_path) as key:

            winreg.SetValueEx(key, None, 0, winreg.REG_SZ, label)

            winreg.SetValueEx(key, "MUIVerb", 0, winreg.REG_SZ, label)

        with winreg.CreateKeyEx(

            winreg.HKEY_CURRENT_USER, child_path + r"\command"

        ) as cmd_key:

            winreg.SetValueEx(cmd_key, None, 0, winreg.REG_SZ, _exe_command(verb))





def unregister_desktop_background_verbs() -> None:

    """Remove DeskTidy desktop background shell verbs."""

    _delete_flat_obsolete()

    _delete_key_tree(f"{_ROOT}\\{_PARENT}")





def sync_desktop_background_verbs(*, enabled: bool) -> None:

    if enabled:

        register_desktop_background_verbs()

    else:

        unregister_desktop_background_verbs()



