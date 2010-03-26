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
    sh '/usr/bin/env bash -c "make clean"' rescue nil
    sh "~/.rvm/bin/memprof_ruby extconf.rb"
    sh '/usr/bin/env bash -c "make"'
  end
  sh "~/.rvm/bin/memprof_ruby spec/memprof_spec.rb"
end

task :mri_x86_64_shared do
  sh '/usr/bin/env bash -c "
  source ~/.rvm/scripts/rvm &&
  rvm install 1.8.7 --reconfigure -C --enable-shared &&
  rvm 1.8.7 --symlink memprof"'
  Rake::Task[:ci_spec].invoke
end

task :mri_x86_64_static do
  sh '/usr/bin/env bash -c "
  source ~/.rvm/scripts/rvm &&
  rvm install 1.8.7 --reconfigure -C --disable-shared &&
  rvm 1.8.7 --symlink memprof"'
  Rake::Task[:ci_spec].invoke
end

task :ree_x86_64_shared do
  sh '/usr/bin/env bash -c "
  source ~/.rvm/scripts/rvm &&
  rvm install ree --reconfigure -C --enable-shared &&
  rvm ree --symlink memprof"'
  Rake::Task[:ci_spec].invoke
end

task :ree_x86_64_static do
  sh '/usr/bin/env bash -c "
  source ~/.rvm/scripts/rvm &&
  rvm install ree --reconfigure -C --disable-shared &&
  rvm ree --symlink memprof"'
  Rake::Task[:ci_spec].invoke
end

