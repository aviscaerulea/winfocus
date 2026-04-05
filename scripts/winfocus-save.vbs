Dim fso, dir, exe
Set fso = CreateObject("Scripting.FileSystemObject")
dir = fso.GetParentFolderName(WScript.ScriptFullName)
exe = fso.BuildPath(dir, "winfocus.exe")
CreateObject("WScript.Shell").Run """" & exe & """ --save", 1, True
