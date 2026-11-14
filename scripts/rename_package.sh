#!/bin/bash
# Rename the app's package everywhere:
#   applicationId com.botwrecomp.android -> com.zbotw.recomp   (the installed app's name)
#   java package  com.botwrecompandroid  -> com.zbotw.recomp   (with it, the JNI names: Java_com_botwrecompandroid_
# The probe app (android/probe) and the test scripts follow. run_ver.sh's adb paths and lsfg's data path too.
set -euo pipefail
cd "$(dirname "$0")/.."

OLD_ID="com.botwrecomp.android"
NEW_ID="com.zbotw.recomp"
OLD_J="com.botwrecompandroid"
NEW_J="com.zbotw.recomp"
OLD_US="com_botwrecompandroid"
NEW_US="com_zbotw_recomp"
OLD_SL="com/botwrecompandroid"
NEW_SL="com/zbotw/recomp"

# 1. the java sources move to their new package directory
APP=android/botwrecompandroid/app/src/main/java
mkdir -p $APP/com/zbotw/recomp
for f in $APP/com/botwrecompandroid/*.java; do
    sed -i "s/^package $OLD_J;/package $NEW_J;/" "$f"
    sed -i "s/$OLD_US/$NEW_US/g" "$f"
    mv "$f" $APP/com/zbotw/recomp/
done
rmdir $APP/com/botwrecompandroid

# 2. the gradle files (namespace, applicationId, the java sourceset path if hardcoded)
sed -i "s/namespace = \"$OLD_J\"/namespace = \"$NEW_J\"/; s/applicationId = \"$OLD_ID\"/applicationId = \"$NEW_ID\"/" \
    android/botwrecompandroid/app/build.gradle.kts
sed -i "s/namespace = \"$OLD_J.probe\"/namespace = \"$NEW_J.probe\"/; s/applicationId = \"$OLD_J.probe\"/applicationId = \"$NEW_J.probe\"/" \
    android/probe/app/build.gradle.kts

# 3. the manifest references (android:name=".Activity" is relative to the namespace - nothing to do) and shortcuts.xml
sed -i "s#$OLD_J\.#$NEW_J.#g; s#$OLD_ID#$NEW_ID#g" android/botwrecompandroid/app/src/main/res/xml/shortcuts.xml

# 4. the runtime's JNI function names (launcher.mm, mods_tab.mm, watch_map.c, platform/android.c, probe.c)
for f in runtime/ui/launcher.mm runtime/ui/mods_tab.mm runtime/watch_map.c runtime/platform/android.c \
         android/probe/app/src/main/cpp/probe.c companion/tests/WatchMapImageTest.java; do
    sed -i "s#Java_$OLD_US#Java_$NEW_US#g; s#$OLD_US#$NEW_US#g; s#$OLD_SL#$NEW_SL#g; s#$OLD_J#$NEW_J#g; s#$OLD_ID#$NEW_ID#g" "$f"
done

# 5. the probe's java package dir
PR=android/probe/app/src/main/java
if [ -d $PR/com/botwrecompandroid/probe ]; then
    mkdir -p $PR/com/zbotw/recomp
    for f in $PR/com/botwrecompandroid/probe/*.java; do
        sed -i "s/^package $OLD_J.probe;/package $NEW_J.probe;/" "$f"
        mv "$f" $PR/com/zbotw/recomp/
    done
    mv $PR/com/zbotw/recomp $PR/com/zbotw/recomp_tmp 2>/dev/null || true
    # package com.zbotw.recomp.probe needs dir com/zbotw/recomp/probe
    mkdir -p $PR/com/zbotw/recomp
    mv $PR/com/zbotw/recomp_tmp/*.java $PR/com/zbotw/recomp/ 2>/dev/null || true
    rm -rf $PR/com/zbotw/recomp_tmp $PR/com/botwrecompandroid
    # the files are now in com/zbotw/recomp but their package line says com.zbotw.recomp.probe: move them one deeper
    mkdir -p $PR/com/zbotw/recomp/probe
    for f in $PR/com/zbotw/recomp/*.java; do mv "$f" $PR/com/zbotw/recomp/probe/; done
fi

# 6. the run scripts' adb paths and the docs' examples
for f in android/run_ver.sh android/run_boot.sh android/run_burst.sh android/run_cfgs.sh android/run_frames.sh \
         android/run_mem.sh android/run_shots.sh android/push_game.sh tools/hot_functions.py; do
    [ -f "$f" ] && sed -i "s/$OLD_ID/$NEW_ID/g; s/$OLD_J/$NEW_J/g" "$f"
done
sed -i "s/$OLD_ID/$NEW_ID/g" runtime/nvn/vulkan/lsfg/lsfg_present.c

# 7. anything left with the old names outside docs/ and .git
echo "--- remaining references (docs excluded) ---"
grep -rn "$OLD_US\|$OLD_J\|$OLD_ID" --include="*.kts" --include="*.java" --include="*.xml" --include="*.c" \
     --include="*.mm" --include="*.h" --include="*.sh" --include="*.py" . 2>/dev/null | grep -v "^\./\.git\|/build/\|^\./docs/" || echo "(none)"
