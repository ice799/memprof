task :spec do
  Dir.chdir('ext') do
    sh "make clean" rescue nil
    sh "ruby extconf.rb"
    sh "make"
  end
  sh "ruby spec/memprof_spec.rb"
end
task :default => :spec

# Should be used like:
# rake --trace ci[1.8.7,shared]

task :ci, [:ruby_type, :lib_option] do |t, args|
  ruby_type, lib_option = args[:ruby_type], args[:lib_option]
  raise "#{ruby_type} is not a supported ruby version" unless ["1.8.6", "1.8.7", "ree"].include?(ruby_type)
  raise "#{lib_option} is not a supported " unless ["shared", "static"].include?(lib_option)

  lib_option = case lib_option
  when "static"
    "--disable-shared"
  when "shared"
    "--enable-shared"
  end

  sh "/usr/bin/env bash -c \"
  source ~/.rvm/scripts/rvm &&
  rvm install #{ruby_type} --reconfigure -C #{lib_option} &&
  rvm #{ruby_type} --symlink memprof &&
  memprof_gem install bacon\""

  Dir.chdir('ext') do
    sh '/usr/bin/env bash -c "make clean"' rescue nil
    sh "~/.rvm/bin/memprof_ruby extconf.rb"
    sh '/usr/bin/env bash -c "make"'
  end
  sh "~/.rvm/bin/memprof_ruby spec/memprof_spec.rb"
end

