# New Feature Merge Request

### New Feature Description
What does this feature do? Does it address a known problem? Describe the reason for this merge. (required)

### Merge Checklist
#### Requester: 
- [ ] [IMB Tests](https://scm.nowlab.cse.ohio-state.edu/mvapich/mvapich2/-/wikis/run_imb) passed (block and cyclic distribution)
- [ ] [MPICH Tests](https://scm.nowlab.cse.ohio-state.edu/snippets/32) passed (block and cyclic distribution)
- [ ] Regressions linked
    * Latest master compared to new feature performance
- [ ] CHANGELOG Updated

#### Reviewer
- [ ] Builds and runs with default build options for ch3:mrail (Frontera, RI2, Lassen)
    * Reasonable effort to address all warnings has been made
    * Passes basic OMB sanity test
- [ ] Builds and runs with default build options for ch3:psm (Stampede2, Quartz)
    * Reasonable effort to address all warnings has been made
    * Passes basic OMB sanity test
- [ ] Builds and runs with default build options for ch3:sock (any)
    * Reasonable effort to address all warnings has been made
- [ ] Builds with new feature disabled (if supported)
    * Reasonable effort to address all warnings has been made
    * Passes basic OMB sanity test
- [ ] Checked for formatting and coding style
    * Four spaces, no tabs
    * ANSI C style comments - "/*"
    * 80 column text width
    * Uses proper [MPICH error handling](https://scm.nowlab.cse.ohio-state.edu/mvapich/mvapich2/-/wikis/error-handling)
    * Spell checked comments

#### Assignee
- [ ] No conflicts with master 
- [ ] Peer reviewed



### Related Issues 
Closes (add any issues this merge is closing here or put N/A)

### Comments/Changelog
What should be added to the CHANGELOG? Relevant file changes? Are there known issues? New configuration or runtime options? (required)

/assign 

/label ~Enhancement ~"Awaiting Validation"
