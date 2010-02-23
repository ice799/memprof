task :spec do
  sh "make -C ext clean && ruby -C ext extconf.rb && make -C ext"
  sh "ruby spec/memprof_spec.rb"
end
task :default => :spec
