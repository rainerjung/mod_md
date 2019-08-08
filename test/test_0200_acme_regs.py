# test mod_md ACMEv1 registrations

import os.path
import re
import sys
import time
import pytest
import json

from datetime import datetime
from shutil import copyfile
from test_base import TestEnv

def setup_module(module):
    print("setup_module: %s" % module.__name__)
    TestEnv.initv1()

def teardown_module(module):
    print("teardown_module:%s" % module.__name__)


class TestAcmeAcc :

    def setup_method(self, method):
        print("setup_method: %s" % method.__name__)
        TestEnv.check_acme()
        TestEnv.clear_store()
 
    def teardown_method(self, method):
        print("teardown_method: %s" % method.__name__)

    # test case: register a new account, vary length to check base64 encoding
    @pytest.mark.parametrize("contact", [
        ("x@not-forbidden.org"), ("xx@not-forbidden.org"), ("xxx@not-forbidden.org")
    ])
    def test_200_000(self, contact):
        run = TestEnv.a2md( ["acme", "newreg", contact], raw=True )
        assert run['rv'] == 0
        m = re.match("registered: (.*)$", run["stdout"])
        assert m
        acct = m.group(1)
        print("newreg: %s" % (m.group(1)))
        # verify account in local store
        self._check_account(acct, ["mailto:" + contact])

    # test case: respect 'mailto:' prefix in contact url
    def test_200_001(self):
        contact = "mailto:xx@not-forbidden.org"
        run = TestEnv.a2md( ["acme", "newreg", contact], raw=True )
        assert run['rv'] == 0
        m = re.match("registered: (.*)$", run["stdout"])
        assert m
        acct = m.group(1)
        self._check_account(acct, [contact])

    # test case: fail on invalid contact url
    @pytest.mark.parametrize("invalidContact", [
        ("mehlto:xxx@not-forbidden.org"), ("no.at.char"), ("with blank@test.com"), ("missing.host@"), ("@missing.localpart.de"), 
        ("double..dot@test.com"), ("double@at@test.com")
    ])
    def test_200_002(self, invalidContact):
        assert TestEnv.a2md( ["acme", "newreg", invalidContact] )['rv'] == 1

    # test case: use contact list
    def test_200_003(self):
        contact = [ "xx@not-forbidden.org", "aa@not-forbidden.org" ]
        run = TestEnv.a2md( ["acme", "newreg"] + contact, raw=True )
        assert run['rv'] == 0
        m = re.match("registered: (.*)$", run["stdout"])
        assert m
        acct = m.group(1)
        # verify account in local store
        self._check_account(acct, ["mailto:" + contact[0], "mailto:" + contact[1]])

    # test case: validate new account
    def test_200_100(self):
        acct = self._prepare_account(["tmp@not-forbidden.org"])
        assert TestEnv.a2md( ["acme", "validate", acct] )['rv'] == 0

    # test case: fail on non-existing account
    def test_200_101(self):
        assert TestEnv.a2md( ["acme", "validate", "ACME-localhost-1000"] )['rv'] == 1

    # test case: report fail on request signing problem
    def test_200_102(self):
        acct = self._prepare_account(["tmp@not-forbidden.org"])
        # modify server's reg url
        jsonFile = TestEnv.path_account(acct)
        with open(jsonFile) as f:
            acctj = json.load(f)
        acctj['url'] = acctj['url'] + "0"
        open(jsonFile, "w").write(json.dumps(acctj))
        assert TestEnv.a2md( ["acme", "validate", acct] )['rv'] == 1

    # test case: register and try delete an account, will fail without persistence
    def test_200_200(self):
        acct = self._prepare_account(["tmp@not-forbidden.org"])
        assert TestEnv.a2md( ["delreg", acct] )['rv'] == 1

    # test case: register and try delete an account with persistence
    def test_200_201(self):
        acct = self._prepare_account(["tmp@not-forbidden.org"])
        assert TestEnv.a2md( ["acme", "delreg", acct] )['rv'] == 0
        # check that store is clean
        # TODO: create a "a2md list accounts" command for this
        run = TestEnv.run(["find", TestEnv.STORE_DIR])
        assert re.match(TestEnv.STORE_DIR, run['stdout'])

    # test case: delete a persisted account without specifying url
    def test_200_202(self):
        acct = self._prepare_account(["tmp@not-forbidden.org"])
        assert TestEnv.run([TestEnv.A2MD, "-d", TestEnv.STORE_DIR, "acme", "delreg", acct] )['rv'] == 0

    # test case: delete, then validate an account
    def test_200_203(self):
        acct = self._prepare_account(["test014@not-forbidden.org"])
        assert TestEnv.a2md( ["acme", "delreg", acct] )['rv'] == 0
        assert TestEnv.a2md( ["acme", "validate", acct] )['rv'] == 1

    def _check_account(self, acct, contact):
        with open(TestEnv.path_account(acct)) as f:
            acctj = json.load(f)
        assert acctj['registration']['contact'] == contact

    def _prepare_account(self, contact):
        run = TestEnv.a2md( ["acme", "newreg"] + contact, raw=True )
        assert run['rv'] == 0
        return re.match("registered: (.*)$", run['stdout']).group(1)

