# Sudo for Windows

This enables running a program or command with elevation (as administrator)
or as a specific user account.

It is modelled after the Linux `sudo` command, and has many of the same flags
and behaviors.  Due to differences between Linux and Windows, some features
of the Linux command are not applicable on Windows and have not been
replicated.

This is written in C++ and uses [Premake](https://premake.github.io) for
creating project files for compiling with Visual Studio or MinGW (or other
toolsets supported by Premake).

```plain
C:\Users\Me> sudo -?
Runs the specified command line with elevation (as super user).

SUDO [options] {command line}

  -?, -h, --help            Display a short help message and exit.
  -b, --background          Run the command in the background.  Interactive
                            commands will likely fail to work properly when
                            run in the background.
  -D dir, --chdir=dir       Run the command in the specified directory.
  -n, --non-interactive     Avoid showing any UI.
  -p text, --prompt=text    Use a custom password prompt.
  -S, --stdin               Write the prompt to stderr and read the password
                            from stdin instead of using the console.
  -u user, --user=user      Run the command as the specified user.
  -V, --version             Print the sudo version string.
  --                        Stop processing options in the command line.

Redirection and pipes work if the symbols are used inside quotes; otherwise
the symbols are interpreted by CMD before they reach sudo.

If you get into an endless loop of spawning sudo.exe, you can hold
Alt+Ctrl+Shift at the same time to cancel.

The custom password prompt can include the following escape sequences:

  %H                Expands to the host name with the domain name.
  %h                Expands to the local host name without the domain name.
  %p or %U          Expands to the name of the user whose password is being
                    requested.
  %u                Expands to the invoking user's account name.
  %%                Expands to a single percent sign.

The password prompt uses the custom prompt string, if provided.  Otherwise it
uses the %SUDO_PROMPT% or a default prompt string.
```
