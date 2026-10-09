Feature: Retention data
    Retention data that cannot be loaded completely

    Scenario: Naemon refuses to start when the retention file cannot be read
        Given the retention file cannot be read
        Then naemon should fail to start
        And the naemon log should contain the retention data could not be loaded completely
        And the retention file should still be there

    Scenario: Naemon refuses to start with damaged retention data when retention_strict_loading is set
        Given I have naemon config retention_strict_loading set to 1
        And I have a damaged retention file
        Then naemon should fail to start
        And the naemon log should contain Not loading it because retention_strict_loading is enabled
        And the retention file should be unchanged

    Scenario: Naemon starts with damaged retention data by default
        Given I have a damaged retention file
        And I start naemon and wait until it is ready
        Then naemon should be running
        And the naemon log should contain is damaged: a line without '=' inside a block on line 10
