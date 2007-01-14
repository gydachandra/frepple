#!/usr/bin/python
#  file     : $URL$
#  revision : $LastChangedRevision$  $LastChangedBy$
#  date     : $LastChangedDate$
#  email    : jdetaeye@users.sourceforge.net

#
# This python script uses the module unittest to run a series of tests.
#
# Each test has its own subdirectory. In the description below it is referred
# to as {testdir}.
# Three categories of tests are supported.
#
#  - Type 1: Compiled executable
#    If an executable file {testdir} or {testdir}.exe is found in the test
#    directory, the executable is run. The compilation/generation of the
#    executable is not handled by the script, but it's typically done by
#    running the command "make check" in the test subdirectory.
#    The test is successful if both:
#      1) the exit code of the program is 0
#      2) the output of the program is identical to the content of the
#         file {testdir}.expect
#
#  - Type 2: Run a Python test script
#    If a file runtest.py is found in the test directory, it is being run
#    and its exit code is used as the criterium for a successful test.
#
#  - Type 3: Process an XML file
#    If a file {testdir}.xml is found in the test directory, the frepple
#    commandline executable is called to process the file.
#    The test is successful if both:
#      1) the exit code of the program is 0
#      2) the generated output files of the program match the content of the
#         files {testdir}.{nr}.expect
#    If a file init.xml is found in the test directory, the test directory
#    is used as the frepple home directory.
#
#  - If the test subdirectory doesn't match the criteria of any of the above
#    types, the directory is considered not to contain a test.
#
# The script can be run with the following arguments on the command line:
#  - ./runtest.py {test}
#    ./runtest.py {test1} {test2}
#    Execute the tests listed on the command line.
#  - ./runtest.py
#    Execute all tests.
#  - ./runtest.py -vcc
#    Execute all tests using the executables compiled with Microsofts'
#    Visual Studio C++ compiler.
#    Tests of type 1 are skipped in this case.
#  - ./runtest.py -bcc
#    Execute all tests using the executables compiled with the Borland
#    C++ compiler.
#    Tests of type 1 are skipped in this case.
#

import unittest, os, os.path, getopt, sys, filecmp, glob, filecmp

debug = False 

def usage():
    # Print help information and exit
    print
    print 'Usage to run all tests:'
    print '  ./runtest.py [-v|--vcc|-b|--bcc|-d|--debug]'        
    print 'Usage with explicit list of tests to run:'
    print '  ./runtest.py [-v|--vcc|-b|--bcc|-d|--debug] {test1} {test2} ...'

def runTestSuite():
    global debug
    # Executable to be used for the tests. Exported as an environment variable.
    # This default executable is the one valid  for GCC cygwin and GCC *nux builds.
    os.environ['EXECUTABLE'] = "../../libtool --mode=execute ../../src/frepple" 
    platform = 'GCC'

    # Directory names for tests and frepple_home
    testdir = os.path.dirname(os.path.abspath(sys.argv[0]))
    os.environ['FREPPLE_HOME'] = os.path.normpath(os.path.join(testdir, '..', 'bin'))
    
    # Parse the command line
    opts = []
    tests = []
    try:
        opts, tests = getopt.getopt(sys.argv[1:], "dvbh", ["debug", "vcc", "bcc", "help"])
    except getopt.GetoptError: usage()
    for o, a in opts:
        if o in ("-v" "--vcc"):
             # Use executables generated by the Visual C++ compiler
             platform = 'VCC'
             os.environ['EXECUTABLE'] = "../../bin/frepple_vcc.exe"; 
        elif o in ("-b" "--bcc"):
             # Use executables generated by the Borland C++ compiler
             platform = 'BCC'
             os.environ['EXECUTABLE'] = "../../bin/frepple_bcc.exe";
        elif o in ("-d", "--debug"):
            debug = True
        elif o in ("-h", "--help"):
            # Print help information and exit
            usage();
            
    # Argh... Special cases for that special platform again...
    if sys.platform == 'cygwin' and platform == 'VCC':
        # Test running with cygwin python but testing the vcc executable
        fo = os.popen("cygpath  --windows " + os.environ['FREPPLE_HOME'])
        os.environ['FREPPLE_HOME'] = fo.readline().strip()
        fo.close()
    if sys.platform == 'win32' and platform == 'GCC':
        # Test running with windows python but testing the cygwin executable
        fo = os.popen("cygpath  --unix " + os.environ['FREPPLE_HOME'])
        os.environ['FREPPLE_HOME'] = fo.readline().strip()
        fo.close()
            
    # Define a list with tests to run
    if len(tests) == 0:
        # No tests specified, so run them all
        subdirs = os.listdir(testdir)
        subdirs.sort()
        for i in subdirs:
            if i == '.svn' or os.path.isfile(i): continue
            tests.append(i)
    else:
        # A list of tests has been specified, and we now validate it
        for i in tests:
            if not os.path.isdir(os.path.join(testdir,i)):
                print "Warning: Test directory " + i + " doesn't exist"
                tests.remove(i)
                
    # Now define the test suite
    AllTests = unittest.TestSuite()
    for i in tests:
        i = os.path.normpath(i)
        tmp = os.path.join(testdir,i,i)
        if os.path.isfile(tmp) or os.path.isfile(tmp + '.exe'):
            # Type 1: (compiled) executable
            if platform == "GCC": AllTests.addTest(freppleTest(i,'runExecutable'))  
        elif os.path.isfile(os.path.join(testdir,i,'runtest.pl')):
            # Type 2: perl script runtest.pl available
            AllTests.addTest(freppleTest(i,'runScript'))
        elif os.path.isfile(tmp + '.xml'):
            # Type 3: input xml file specified
            AllTests.addTest(freppleTest(i,'runXML'))
        else:
            # Undetermined - not a test directory
            print "Warning: Unrecognized test in directory " + i
    
    # Finally, run the test suite now
    print "Running", AllTests.countTestCases(), \
         "tests from directory", testdir, \
         "with FREPPLE_HOME", os.environ['FREPPLE_HOME'] 
    unittest.TextTestRunner(verbosity=2).run(AllTests)
       

class freppleTest (unittest.TestCase):
    def __init__(self, directoryname, methodName):
        self.subdirectory = directoryname
        super(freppleTest,self).__init__(methodName)
    
    def setUp(self):
        os.chdir(os.path.join(os.environ['FREPPLE_HOME'], '..', 'test', self.subdirectory))
                 
    def shortDescription(self): 
        ''' Use the directory name as the test name.'''
        return self.subdirectory
    
    def runExecutable(self):
        '''Running a compiled executable'''
        # Run the command and verify exit code
        try:
            self.assertEqual(os.system("./" + self.subdirectory + ">test.out 2>&1"),0)
            self.assertEqual(filecmp.cmp("test.out",self.subdirectory + ".expect"),True)
        except KeyboardInterrupt:
            # The test has been interupted, which counts as a failure
            self.assertFalse     

    def runScript(self): 
        '''Running a test script'''
        pass
        '''
        try:
            self.assertEqual(os.system("perl runtest.pl"),0)
        except KeyboardInterrupt:
            # The test has been interupted, which counts as a failure
            self.assertFalse     
       '''
       
    def runXML(self):
        '''Running the command line tool with an XML file as argument.'''
        global debug
        
        # Delete previous output
        self.output = glob.glob("output.*.xml")
        self.output.extend(glob.glob("output.*.txt"))
        self.output.extend(glob.glob("output.*.tmp"))
        for i in self.output: os.remove(i)  
        
        # Update FREPPLE_HOME if required
        oldhome = os.environ['FREPPLE_HOME']
        if os.path.isfile('init.xml'):
            os.environ['FREPPLE_HOME'] = os.path.join(os.environ['FREPPLE_HOME'], '..', 'test', self.subdirectory)           
        
        # Run the executable
        try:
            if debug:
                print ''
                print 'output:'
                self.assertEqual(os.system(os.environ['EXECUTABLE'] + " -validate " + self.subdirectory + ".xml"),0)
                print ''
            else:
                self.assertEqual(os.system(os.environ['EXECUTABLE'] + " -validate " + self.subdirectory + ".xml >/dev/null 2>&1"),0)
        except KeyboardInterrupt:
            # The test has been interupted, which counts as a failure
            self.assertFalse     
        os.environ['FREPPLE_HOME'] = oldhome
        
        # Now check the output file, if there is an expected output given
        nr = 1;
        while os.path.isfile(self.subdirectory + "." + str(nr) + ".expect") or \
           os.path.isfile(self.subdirectory + "." + str(nr) + "s.expect"):
            if os.path.isfile("output."+str(nr)+"s.xml"):
                if debug: print "Summarizing the plan", nr
                #system("grep -v OPERATION output.${nr}s.xml >output.${nr}s.tmp");
                #system("grep OPERATION output.${nr}s.xml | sort >>output.${nr}s.tmp");
                #system("grep -v PROBLEM output.${nr}s.tmp >output.${nr}s.txt");
                #system("grep PROBLEM output.${nr}s.tmp >>output.${nr}s.txt");
                #print "Comparing expected and actual output $nr\n";
                #system("diff -w $subdir.${nr}s.expect output.${nr}s.txt");
                #return if $? ne 0;
            elif os.path.isfile("output."+str(nr)+".xml"):
                if debug: print "Comparing expected and actual output", nr
                if not os.path.isfile("output."+str(nr)+".xml"):
                    self.assertFalse('Missing planner output file')
                self.assertEqual( \
                    filecmp.cmp(self.subdirectory + "." + str(nr) + ".expect", \
                                "output."+str(nr)+".xml"), \
                    True, \
                    "Difference in output " + str(nr))
                #system("diff -w $subdir.${nr}.expect output.${nr}.xml");
                #return if $? ne 0;
            else:
                self.assertFalse('Missing planner output file')
            nr += 1;
        
# If the file is processed as a script, run the test suite.
# Otherwise, only define the methods.                
if __name__ == "__main__": runTestSuite()
