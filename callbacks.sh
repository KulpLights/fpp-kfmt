#!/bin/bash
#
# This file must stay in the repo ROOT. Unlike fpp_install.sh, FPP looks for the
# callbacks script only at <plugindir>/<name>/callbacks[.sh|.pl|.php|.py] with no
# scripts/ fallback (PluginManager::loadUserPlugin), and the "c++" it prints for
# --list is what makes FPP dlopen the shared library at all. Moving it here would
# silently stop the plugin from loading.

for var in "$@"
do
	case $var in
		-l|--list)
			echo "c++";
            exit 0;
		;;
		-h|--help)
			usage
			exit 0
		;;
		-v|--version)
			printf "%s, version %s\n" "$PROGRAM_NAME" "$PROGRAM_VERSION"
			exit 0
		;;
		--)
			# no more arguments to parse
			break
		;;
		*)
			printf "Unknown option %s\n" "$var"
			exit 1
		;;
	esac
done

