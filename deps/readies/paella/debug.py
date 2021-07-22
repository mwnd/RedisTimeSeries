
import os

#----------------------------------------------------------------------------------------------

pydebug = os.environ.get('PYDEBUG', '')
if pydebug == '':
    pydebug = os.environ.get('BB', '')
    if pydebug != '':
        os.environ['PYDEBUG'] = pydebug
if pydebug == '1':
    try:
        from pudb import set_trace as bb
    except ImportError:
        try:
            from ipdb import set_trace as bb
        except ImportError:
            from pdb import set_trace as bb
elif pydebug == 'pudb':
    from pudb import set_trace as bb
elif pydebug == 'pdb':
    from pdb import set_trace as bb
elif pydebug == 'ipdb':
    from ipdb import set_trace as bb
else:
	def bb(): pass

#----------------------------------------------------------------------------------------------
