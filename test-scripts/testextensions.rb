# Don't forget to fire up Dbmail's IMAP server before starting this script:
#  sudo dbmail-imapd
# Run this file with:
#  ruby test-scripts/testextensions.rb
require "test/unit"
require "net/imap"

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

  def imap_conn
    #Net::IMAP.debug = true
    @imap = Net::IMAP.new(IMAP_SERVER, IMAP_PORT) # without ssl
  end

  def imap_logout
    @imap.logout
  rescue Net::IMAP::ResponseParseError # catch \r\n after BYE responses (TODO: is it a failure of Net::IMAP or Dbmail??)
    # do nothing
  end

  def test_greeting
    imap_conn
    assert_match(/^\* OK \[.*?\] dbmail \d+(\.\d+)+ \(compiled on \w+ \d{1,2} \d{4} \d{1,2}:\d{2}:\d{2}\) ready\.\r\n$/, @imap.greeting.raw_data, 'Imap greeting contains compilation info')
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
end