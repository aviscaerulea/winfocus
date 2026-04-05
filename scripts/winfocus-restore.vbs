Dim fso, dir, exe
Set fso = CreateObject("Scripting.FileSystemObject")
dir = fso.GetParentFolderName(WScript.ScriptFullName)
exe = fso.BuildPath(dir, "winfocus.exe")
CreateObject("WScript.Shell").Run """" & exe & """ --restore", 1, True
