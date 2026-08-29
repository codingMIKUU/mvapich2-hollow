#!/bin/bash

# to be sourced in the prescript and set the branch name in the environment
if [ $CI_MERGE_REQUEST_TARGET_BRANCH_NAME ] 
then
    BRANCH=$CI_MERGE_REQUEST_TARGET_BRANCH_NAME
elif [ $PIPELINE_BRANCH_TYPE ]
then
    BRANCH=$PIPELINE_BRANCH_TYPE
fi

echo Setting up for branch: $BRANCH

# TODO: add logic for triggering a PSM pipeline
case $BRANCH in
    "master" )
        export BRANCH   
        export BUILDER="MR_mrail_master"
        mkdir .buildbot
        echo 'try_builders = ["MR_mrail_master", "MR_psm_master", "MR_sock_master"]' > .buildbot/options
        ;;
    "master-x" | "x" )
        export BRANCH="master-x"
        export BUILDER="MR_mrail_x"
        mkdir .buildbot
        echo 'try_builders = ["MR_mrail_x", "MR_psm_x", "MR_sock_x"]' > .buildbot/options
        ;;
    "gdr" | "next-gdr" )
        export BRANCH="next-gdr"
        export BUILDER="MR_mrail_gdr"
        mkdir .buildbot
        echo 'try_builders = ["MR_mrail_nvidia_gdr", "MR_mrail_amd_gdr", "MR_sock_gdr"]' > .buildbot/options
        ;;
    * ) 
        export BRANCH="invalid"
        export BUILDER="invalid"
        echo "$BRANCH does not match a valid pipeline branch"
        exit 1
        ;;
esac
