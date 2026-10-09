Feature: Retention data
    A retention file that cannot be loaded completely

    Scenario: Naemon refuses to start with damaged retention data when retention_strict_loading is set
        Given I have naemon config retention_strict_loading set to 1
        And I have a damaged retention file
        Then naemon should fail to start
        And the naemon log should contain retention_strict_loading is enabled
        And the retention file should be unchanged

    Scenario: Naemon starts with damaged retention data by default
        Given I have a damaged retention file
        And I start naemon and wait until it is ready
        Then naemon should be running
        And the naemon log should contain is damaged: a line without '=' inside a block on line 10
