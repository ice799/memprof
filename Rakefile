task :spec do
  Dir.chdir('ext') do
    sh "make clean" rescue nil
    sh "ruby extconf.rb"
    sh "make"
  end
  sh "ruby spec/memprof_spec.rb"
end
task :default => :spec
