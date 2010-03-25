require 'rubygems'
require 'bacon'
Bacon.summary_on_exit

describe "MemprofUploader" do

  it "should display help output with -h" do
    output = `ruby bin/memprof -h`
    output.should =~ /Memprof Uploader/
    output.should =~ /Usage:/
    $?.exitstatus.should == 0
  end

  it "should fail without a pid being passed" do
    output = `ruby bin/memprof -n SomeLabel -k abcdef`
    output.should =~ /Missing PID!/
    $?.exitstatus.should == 1
  end

  it "should fail without a name being passed" do
    output = `ruby bin/memprof -p 123 -k abcdef`
    output.should =~ /Missing name!/
    $?.exitstatus.should == 1
  end

  it "should fail without an API key" do
    output = `ruby bin/memprof -p 123 -n SomeLabel`
    output.should =~ /Missing API key!/
    $?.exitstatus.should == 1
  end

  it "should fail with an invalid pid" do
    output = `ruby bin/memprof -p 99999999 -n Label -k abcdef`
    output.should =~ Regexp.new("No such process 99999999!")
    $?.exitstatus.should == 1
  end

  it "should fail when the target process does not create a new file within 5 sec" do
    pid = fork { sleep 5; exit! }
    Process.detach(pid)
    output = `ruby bin/memprof -p #{pid} -n Label -k abcdef`
    output.should =~ Regexp.new("Waiting 5 seconds for process #{pid} to create a new dump...")
    output.should =~ Regexp.new("Timed out after waiting 5 seconds")
    $?.exitstatus.should == 1
  end

  it "should WORK and wait for a dump to complete if it's IN_PROGRESS" do
    pid = fork {
      # create a fake file
      filename = "/tmp/memprof-#{Process.pid}-#{Time.now.to_i}.json.IN_PROGRESS"
      # simulate dump in progress
      trap("INFO") { File.open(filename, "w") {|f| f.write("foo") }; sleep 1 }
      # should get signaled somewhere in here and execute the handler before exiting.
      sleep 5
      # rename the file to simulate completion of the dump writeout.
      File.rename(filename, filename.sub(/\.IN_PROGRESS/, ""))
      exit!
      }
    Process.detach(pid)
    output = `ruby bin/memprof -p #{pid} -n Label -k abcdef -t`
    output.should =~ Regexp.new("Waiting 5 seconds for process #{pid} to create a new dump...")
    output.should =~ Regexp.new("Found file /tmp/memprof-#{pid}-\\d*.json\\.?\\w*")
    output.should =~ Regexp.new("Dump in progress. Waiting 60 seconds for it to complete...")
    output.should =~ Regexp.new("Finished!")
    file = output.slice(Regexp.new("/tmp/memprof-#{pid}-\\d*.json\\.?\\w*"))
    # make sure both files are gone
    File.exist?(file).should == false
    File.exist?(file.sub(/\.IN_PROGRESS/, "") + ".gz").should == false
    $?.exitstatus.should == 0
  end

  it "should WORK and delete the dump file after it's done, by default" do
    pid = fork {
      require File.dirname(__FILE__) + "/../lib/memprof/signal"
      # should get signaled somewhere in here and execute the handler before exiting.
      sleep 5
      exit!
    }
    Process.detach(pid)
    sleep 2
    output = `ruby bin/memprof -p #{pid} -n TestDump -k abcdef -t`
    output.should =~ Regexp.new("Waiting 5 seconds for process #{pid} to create a new dump...")
    output.should =~ Regexp.new("Found file /tmp/memprof-#{pid}-\\d*.json\\.?\\w*")
    output.should =~ Regexp.new("Finished!")
    file = output.slice(Regexp.new("/tmp/memprof-#{pid}-\\d*.json\\.?\\w*"))
    # make sure both files are gone
    File.exist?(file).should == false
    File.exist?(file.sub(/\.IN_PROGRESS/, "") + ".gz").should == false
    $?.exitstatus.should == 0
  end

  it "should WORK and leave the dump file after it's done, with --no-delete" do
    pid = fork {
      require File.dirname(__FILE__) + "/../lib/memprof/signal"
      # should get signaled somewhere in here and execute the handler before exiting.
      sleep 5
      exit!
    }
    Process.detach(pid)
    sleep 2
    output = `ruby bin/memprof -p #{pid} -n TestDump -k abcdef -t --no-delete`
    output.should =~ Regexp.new("Waiting 5 seconds for process #{pid} to create a new dump...")
    output.should =~ Regexp.new("Found file /tmp/memprof-#{pid}-\\d*.json\\w*")
    output.should =~ Regexp.new("Finished!")
    file = output.slice(Regexp.new("/tmp/memprof-#{pid}-\\d*.json\\.?\\w*"))
    # Make sure it deleted the temporary one
    if file =~ /\.IN_PROGRESS/
      File.exist?(file).should == false
    end
    # make sure it left the completed one
    file = file.sub(/\.IN_PROGRESS/, "") + ".gz"
    File.exist?(file).should == true
    File.delete(file)
    $?.exitstatus.should == 0
  end

end