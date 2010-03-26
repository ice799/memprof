task :spec do
  Dir.chdir('ext') do
    sh "make clean" rescue nil
    sh "ruby extconf.rb"
    sh "make"
  end
  sh "ruby spec/memprof_spec.rb"
end
task :default => :spec

task :ci_spec do
  Dir.chdir('ext') do
    sh "make clean" rescue nil
    sh "memprof_ruby extconf.rb"
    sh "make"
  end
  sh "memprof_ruby spec/memprof_spec.rb"
end

task :x86_64_shared do
  sh "rvm install 1.8.7 --force -C --enable-shared"
  sh "rvm 1.8.7 --symlink memprof"
  Rake::Task[:ci_spec].invoke
end

task :x86_64_static do
  sh "rvm install 1.8.7 --force -C --disable-shared"
  sh "rvm 1.8.7 --symlink memprof"
  Rake::Task[:ci_spec].invoke
end

