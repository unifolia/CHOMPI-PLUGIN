Copies of older CHIMPO builds that this installer moved out of a plug-in folder
are kept here, under removed installs/<date>/<user>/.

A CHIMPO outside /Library (an early test build in ~/Library, for instance)
registers the same AU, VST3 and CLAP component ids as the installed one, so a
host can show two CHIMPOs or none. With these out of the plug-in folders, the
host loads exactly one.

Delete the removed installs folder whenever you no longer want the old builds.
The installed CHIMPO does not read anything from here.
