#!/usr/bin/ruby1.9.1
# Don't forget to fire up Dbmail's IMAP server before starting this script:
#  sudo dbmail-imapd
# Run this file with:
#  ruby test-scripts/testextensions.rb
require "test/unit"
require "net/imap"
# load the time extensions (for Time.now.prev_month)
require "active_support/all"

class MyIMAP < Net::IMAP
  #@@debug = true

  # we need to do the exact reverse of format_internal()
  #  - we need to handle cases in the reverse order
  #  - we have to execute the reverse operations
  def format_imap_to_internal(data)
    if data.include?(',')
      return data.split(",").map{|x| format_imap_to_internal(x)}
    elsif data =~ /^(\d+):(\d+)$/
      return format_imap_to_internal($1)..format_imap_to_internal($2)
    elsif data =~ /^\d+$/
      return data.to_i
    elsif data == "*"
      return data
    end
  end

  def uid_copy_with_new_uid(uid, target)
    res = self.uid_copy(uid, target)
    code = res.data.code
    if code.name == 'COPYUID'
      new_uid = code.data.split(" ")[2]
      if new_uid
        new_uid = format_imap_to_internal(new_uid)
        return new_uid
      end
    end
    return nil
  end

  def uid_delete_without_expunge(uid, trash)
    new_uid = self.uid_copy_with_new_uid(uid, trash)
    self.uid_store(uid, "+FLAGS", [:Deleted])
    self.close
    self.select(trash)
    self.uid_store(new_uid, "+FLAGS", [:Deleted])
    self.close
  end
end

class TestImap < Test::Unit::TestCase
  # Set up some configuration variables
  IMAP_SERVER = 'localhost'
  IMAP_PORT = 143
  # Set up Dbmail's settings to use LDAP.
  # And then add the login credentials that you would like to test here.
  IMAP_LOGINS = [
    {:username => 'username', :password => 'password'},
    {:username => 'username', :password => 'password'}
  ]
  IMAP_COPYTO = 'Test'

  def imap_conn
    Net::IMAP.debug = true
    @imap = MyIMAP.new(IMAP_SERVER, IMAP_PORT) # without ssl
  end

  def imap_logout
    @imap.logout
  rescue Net::IMAP::ResponseParseError # catch \r\n after BYE responses (TODO: is it a failure of Net::IMAP or Dbmail??)
    # do nothing
  end

  def test_greeting
    imap_conn
    res = @imap.greeting
    assert_match(/(^|[^\w])UIDPLUS([^\w]|$)/, res.data.code.data, 'Imap greeting contains UIDPLUS')
    assert_match(/^ dbmail \d+(\.\d+)+ \(compiled on \w+ \d{1,2} \d{4} \d{1,2}:\d{2}:\d{2}\) ready\.$/, res.data.text, 'Imap greeting contains compilation info')
    imap_logout
  end

  def test_logins
    # test that login doesn't fail for all test users
    IMAP_LOGINS.each do |x|
      error = nil
      begin
        imap_conn
        @imap.login(x[:username], x[:password])
        imap_logout
      rescue Exception => e
        error = e
      end
      assert_kind_of(NilClass, error, 'Login was successful for '+x[:username])
    end
  end

  def test_append
    imap_conn
    # take the first imap user
    user = IMAP_LOGINS[0]
    @imap.login(user[:username], user[:password])
    # check for regressions, see whether noop returns what it was returning before the extensions
    res = @imap.noop
    assert_equal("NOOP completed", res.data.text, 'NOOP regression test')
    # check what's the uid_next value and uidvalidity
    @imap.select("inbox")
    old_uid_validity = @imap.responses["UIDVALIDITY"][-1]
    old_uid_next = @imap.responses["UIDNEXT"][-1]
    # save a new message
    res = @imap.append("inbox", <<EOF.gsub(/\n/, "\r\n"), [:Seen], Time.now)
Subject: hello
From: test@moveoneinc.com
To: test@moveoneinc.com

hello world
EOF
    code = res.data.code
    # check the format of the answer
    assert_equal("APPENDUID", code.name)
    uid_validity, uid_set = code.data.split(" ")
    assert_match(/^\d+$/, uid_validity)
    assert_match(/^\d+$/, uid_set)
    #The next line would be necessary in case of multiappend
    #assert_match(/^\d+(:\d+)?(,\d+(:\d+)?)*$/, uid_set) # based on the BNF in rfc4315
    # check whether the uid_validity and uid_next match
    assert_equal(old_uid_validity, uid_validity.to_i, 'Uidvalidity matches')
    assert(old_uid_next.to_i <= uid_set.to_i, 'The appended message\'s uid is higher than the last uid_next value')
    imap_logout
  end

  def test_copy
    imap_conn
    # take the first imap user
    user = IMAP_LOGINS[0]
    @imap.login(user[:username], user[:password])

    # check for regressions, see whether noop returns what it was returning before the extensions
    res = @imap.noop
    assert_equal("NOOP completed", res.data.text, 'NOOP regression test')
    # check what's the uid_next value and uidvalidity in the target folder
    @imap.select(IMAP_COPYTO)
    old_uid_validity = @imap.responses["UIDVALIDITY"][-1]
    old_uid_next = @imap.responses["UIDNEXT"][-1]

    # switch to the inbox folder
    @imap.select("inbox")
    # find messages that were sent in the last month
    # Set up the search keys
    search_keys = ['SENTSINCE', Time.now.prev_month.strftime("%e-%b-%Y")]
    # Search on imap, returns the uids
    uids = @imap.uid_search(search_keys)
    # Get the last 4 messages (or less)
    uids = uids.slice(uids.size > 4 ? uids.size - 4 : 0, 4) # do it this way, because slice returns nil for indexes out of range
    # assert that there are any uids, if this fails it's rather the test data's "bug" than the application's
    assert(!uids.empty?, 'There are uids')

    res = @imap.uid_copy(uids, IMAP_COPYTO)
    code = res.data.code

    assert_respond_to(code, 'name', 'Response code responds to name')
    assert_respond_to(code, 'data', 'Response code responds to data')
    # check the format of the answer
    assert_equal("COPYUID", code.name)
    uid_validity, orig_uids, new_uids = code.data.split(" ")
    assert_match(/^\d+$/, uid_validity)
    assert_match(/^\d+(:\d+)?(,\d+(:\d+)?)*$/, orig_uids) # based on the BNF in rfc4315
    assert_match(/^\d+(:\d+)?(,\d+(:\d+)?)*$/, new_uids) # based on the BNF in rfc4315
    orig_uids = @imap.format_imap_to_internal(orig_uids)
    new_uids = @imap.format_imap_to_internal(new_uids)

    assert_equal(old_uid_validity, uid_validity.to_i, 'Uidvalidity matches')
    assert_equal(uids.sort, orig_uids.sort, 'Old uids returned by UID COPY match the ones that we actually copied')
    assert_equal(orig_uids.size, new_uids.size, 'Old and new uids have the same number of elements')
    new_uids.each do |x|
      # check whether all new uids are above the last seen uid_next value
      assert(old_uid_next.to_i <= x.to_i, x.to_s+': the copied message\'s uid is higher than the last uid_next value ('+old_uid_next.to_s+')')
    end

    imap_logout
  end

  def test_mdnsent
    imap_conn
    # take the first imap user
    user = IMAP_LOGINS[0]
    @imap.login(user[:username], user[:password])

    # check PERMANENTFLAGS response code
    @imap.select("inbox")
    assert(@imap.responses["PERMANENTFLAGS"][-1].include?("$MDNSent"), '$MDNSent in PERMANENTFLAGS: '+@imap.responses["PERMANENTFLAGS"][-1].inspect)

    # append a new message without $MDNSent
    # save a new message
    res = @imap.append("inbox", <<EOF.gsub(/\n/, "\r\n"), [:Seen], Time.now)
Subject: hello mdnsent test
From: test@moveoneinc.com
To: test@moveoneinc.com

hello world!
this is an mdnsent test.
EOF
    code = res.data.code
    uid = code.data.split(" ")[1].to_i
    
    # try searching for "NOT $MDNSent" and the appended uid
    # result should be the expected uid
    check_search_response(['UID', uid, 'NOT', '$MDNSent'], [uid])
    
    # try storing $MDNSent flag on last message
    assert_nothing_raised(Net::IMAP::NoResponseError) { @imap.uid_store(uid, "+FLAGS", ['$MDNSent']) }

    # try searching for "$MDNSent" and the uid
    # result should be the expected uid
    check_search_response(['UID', uid, '$MDNSent'], [uid])
    # try searching for "NOT $MDNSent" and the appended uid
    # result should be empty
    check_search_response(['UID', uid, 'NOT', '$MDNSent'], [])

    # try fetching the message and check for flags
    check_fetch_response_for_flag(uid, '$MDNSent')

    # try removing the MDNSent flag => should fail
    assert_raise(Net::IMAP::BadResponseError) { @imap.uid_store(uid, "-FLAGS", ['$MDNSent']) }

    # try copying the message => MDNSent should stay
    new_uid = @imap.uid_copy_with_new_uid(uid, IMAP_COPYTO)
    @imap.select(IMAP_COPYTO)
    # Check for permanentflags again
    assert(@imap.responses["PERMANENTFLAGS"][-1].include?("$MDNSent"), '$MDNSent in PERMANENTFLAGS: '+@imap.responses["PERMANENTFLAGS"][-1].inspect)
    # and the MDNSent flag
    check_fetch_response_for_flag(new_uid, '$MDNSent')
    # try searching for "$MDNSent" and the uid
    # result should be the expected uid
    check_search_response(['UID', new_uid, '$MDNSent'], [new_uid])

    @imap.select("inbox")
    # append a new message with $MDNSent
    # save a new message
    res = @imap.append("inbox", <<EOF.gsub(/\n/, "\r\n"), [:Seen, '$MDNSent'], Time.now)
Subject: hello mdnsent append test
From: test@moveoneinc.com
To: test@moveoneinc.com

hello world!
this is an mdnsent append test.
EOF
    code = res.data.code
    uid = code.data.split(" ")[1].to_i
    # retrieve the flags and check MDNSent is there
    check_fetch_response_for_flag(uid, '$MDNSent')
    # try searching for "$MDNSent" and the uid
    # result should be the expected uid
    check_search_response(['UID', uid, '$MDNSent'], [uid])

  end

  private
  def check_search_response(search_keys, expected_uids)
    # Search on imap, returns the uids
    uids = @imap.uid_search(search_keys)
    # Assert result uids
    assert_equal(expected_uids.sort, uids.sort, 'Search result uids match the expected')
  end

  def check_fetch_response_for_flag(uid, flag)
    # try fetching the message and check for flags
    res = @imap.uid_fetch(uid, "FLAGS")
    checked_uid = false
    # check for all fetch answers, because server may return data for some other messages too
    res.each do |x|
      # run the flag-check just for the expected uid
      if x['attr']['UID'] == uid
        checked_uid = true
        assert(x['attr']['FLAGS'].include?(flag), "#{flag} in returned flags: "+x['attr'].inspect)
      end
    end
    # ensure there has been any check
    assert_equal(true, checked_uid, 'There has been any check for uid in fetch answer')
  end
end