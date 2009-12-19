require File.dirname(__FILE__) + "/../ext/memprof"

require 'rubygems'
require 'bacon'
Bacon.summary_on_exit

require 'tempfile'

describe Memprof do
  @tempfile = Tempfile.new('memprof_spec')

  def filename
    @tempfile.path
  end

  def filedata
    File.read(filename)
  end

  should 'print stats to a file' do
    Memprof.start
    "abc"
    Memprof.stats(filename)

    filedata.strip.should == "1 #{__FILE__}:#{__LINE__-3}:String"
  end

  should 'collect stats via #track' do
    Memprof.track(filename) do
      "abc"
    end

    filedata.should =~ /1 #{__FILE__}:#{__LINE__-3}:String/
  end
end

